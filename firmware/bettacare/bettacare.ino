/**
 * BettaCare — firmware do controlador do aquário.
 *
 * Dois núcleos, com uma divisão que não é estilística:
 *
 *   núcleo 1 (este arquivo)  controle local — botão, temperatura, ventoinha,
 *                            automação por horário. Nunca bloqueia.
 *   núcleo 0 (net_task)      Wi-Fi, POST de telemetria, NTP, OTA.
 *
 * No firmware antigo tudo isso vivia num `loop()` só, e o handshake TLS do
 * MQTT bloqueava por até 6 segundos — então uma instabilidade de rede travava
 * o botão físico e a histerese da ventoinha ao mesmo tempo. Trocar MQTT por
 * HTTP não resolveria: HTTP bloqueia igual. O que resolve é a separação.
 *
 * O aquário funciona igual com ou sem servidor. Sem rede, a luz segue o
 * horário do último `config` conhecido e a ventoinha segue a temperatura.
 */

#include "api_client.h"
#include "app_state.h"
#include "config.h"
#include "device_config.h"
#include "event_log.h"
#include "fan.h"
#include "light.h"
#include "net_task.h"
#include "rtc_manager.h"
#include "temperature.h"
#include "web_server.h"
#include "wifi_manager.h"

/** Período do laço de controle. 200 ms é imperceptível num botão físico. */
static const uint32_t LOOP_PERIOD_MS = 200;

/**
 * Executa os comandos que a task de rede enfileirou.
 *
 * Toda ação é **estado desejado**, nunca alternância — reenvio precisa ser
 * inofensivo. Cada comando é confirmado (ou recusado com código) de volta,
 * e é essa confirmação que vira a mensagem de erro que o usuário lê no app.
 */
static void _drain_commands() {
  PendingCommand cmd;
  while (app_state_pop_command(cmd)) {
    switch (cmd.kind) {
      case CMD_LIGHT_SET:
        light_set(cmd.on, LIGHT_SRC_COMMAND);
        // O GPIO foi escrito. Se a lâmpada não acender, o servidor descobre
        // pela divergência entre o comandado e o reportado — aqui não há como
        // saber, porque o SSR não tem retorno.
        app_state_push_ack(cmd.id, true, nullptr);
        break;

      case CMD_FAN_SET_SPEED:
        fan_set_speed(cmd.percent);
        app_state_push_ack(cmd.id, true, nullptr);
        break;

      case CMD_FAN_SET_MODE:
        fan_set_mode_auto(cmd.mode_auto);
        app_state_push_ack(cmd.id, true, nullptr);
        break;

      case CMD_CONFIG_APPLY:
        // A configuração em si já foi aplicada pela task de rede ao ler a
        // resposta. O que falta é reavaliar a janela horária agora, em vez de
        // esperar a próxima virada de período.
        rtc_reapply_schedule();
        app_state_push_ack(cmd.id, true, nullptr);
        break;

      case CMD_DEVICE_REBOOT:
        // Confirma antes de reiniciar: depois do restart não haveria quem
        // confirmasse, e o comando expiraria como se tivesse falhado.
        app_state_push_ack(cmd.id, true, nullptr);
        event_log(SEV_WARN, COMP_SYSTEM, "system.reboot_requested",
                  "Reinicio solicitado pelo servidor");
        delay(1500);   // dá tempo de o próximo POST levar o ack
        ESP.restart();
        break;

      case CMD_UNKNOWN:
      default:
        app_state_push_ack(cmd.id, false, "cmd.unsupported");
        break;
    }
  }
}

/** Monta o retrato do estado que a task de rede vai enviar. */
static void _publish_snapshot() {
  DeviceSnapshot s = {};

  s.light_on     = light_get_state();
  s.light_source = light_get_source();

  s.temp_available = temperature_available();
  s.temp_valid     = temperature_is_fresh();
  s.temp_celsius   = temperature_read();
  s.temp_age_ms    = temperature_age_ms();

  s.fan_on            = fan_is_on();
  s.fan_speed_percent = (uint8_t)fan_get_speed_percent();
  s.fan_rpm           = (uint16_t)fan_get_rpm();
  s.fan_mode          = fan_get_mode_report();

  s.rtc_available  = rtc_available();
  s.rtc_lost_power = rtc_lost_power();
  strncpy(s.rtc_time, rtc_get_time_str().c_str(), sizeof(s.rtc_time) - 1);
  s.rtc_time[sizeof(s.rtc_time) - 1] = '\0';

  s.wifi_rssi       = wifi_rssi();
  s.wifi_reconnects = wifi_reconnect_count();
  strncpy(s.wifi_ip, wifi_local_ip().c_str(), sizeof(s.wifi_ip) - 1);
  s.wifi_ip[sizeof(s.wifi_ip) - 1] = '\0';

  s.uptime_ms = millis();

  app_state_publish(s);
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  // Ordem importa: o log precisa existir antes de qualquer módulo querer
  // registrar algo, e a config antes de qualquer módulo querer lê-la.
  event_log_init();
  app_state_init();
  device_config_init();

  light_init();
  temperature_init();
  fan_init();
  rtc_init();

  api_client_init();

  // Tudo que bloqueia vai para o núcleo 0 a partir daqui.
  wifi_connect();
  webserver_init();
  net_task_start();

  event_log(SEV_INFO, COMP_SYSTEM, "system.ready",
            "Controle local ativo; luz %s-%s, ventoinha %.1f/%.1fC",
            device_config_on_time().c_str(), device_config_off_time().c_str(),
            device_config_snapshot().fan_trigger_c,
            device_config_snapshot().fan_off_c);
}

void loop() {
  uint32_t inicio = millis();

  _drain_commands();

  light_check_button();    // debounce de 50 ms
  temperature_update();    // conversão não-bloqueante do DS18B20
  fan_update();            // pot, histerese, cooldown, tacômetro
  rtc_check_automation();  // age só na transição de período

  _publish_snapshot();

  // Período fixo em vez de `delay(200)` cru: o trabalho acima leva tempo
  // variável, e sem descontá-lo o intervalo real derivaria.
  uint32_t gasto = millis() - inicio;
  if (gasto < LOOP_PERIOD_MS) delay(LOOP_PERIOD_MS - gasto);
}
