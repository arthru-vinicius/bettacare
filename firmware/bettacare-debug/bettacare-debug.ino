/**
 * BettaCare — firmware do controlador do aquário, **versão de debug**.
 *
 * É o firmware de produção (`firmware/bettacare`) inteiro, mais a
 * instrumentação de `debug_probe.h`: o endpoint `/debug`, as chaves que
 * desligam subsistemas e os ganchos que injetam falhas. Grave este só para
 * investigar; o uso normal é o de produção. Ver `README.md` nesta pasta.
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
#include "debug_probe.h"
#include "device_config.h"
#include "diagnostics.h"
#include "event_log.h"
#include "fan.h"
#include "feeder_link.h"
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
        //
        // Quem decide QUANDO reiniciar é a task de rede, não um temporizador
        // fixo aqui: só ela sabe quando o `ack` realmente saiu num POST bem-
        // sucedido (UPGRADE/03, F2). Ver `app_state_request_reboot`.
        app_state_push_ack(cmd.id, true, nullptr);
        event_log(SEV_WARN, COMP_SYSTEM, "system.reboot_requested",
                  "Reinicio solicitado pelo servidor");
        app_state_request_reboot();
        break;

      case CMD_DEVICE_DIAGNOSE:
        // Roda aqui, no núcleo de controle, porque sonda I²C e 1-Wire — os
        // mesmos barramentos que o loop já usa, e portanto sem disputa. A
        // conversão completa do DS18B20 alonga esta volta (163 ms medidos
        // aqui; até ~1 s com um sensor que use os 750 ms do datasheet), o que
        // não perde toque: o botão é amostrado por um timer próprio, fora do
        // loop (ver `light.cpp`).
        diagnostics_run(cmd.id);
        app_state_push_ack(cmd.id, true, nullptr);
        break;

      case CMD_FEEDER_FEED_NOW:
        // Sem o módulo respondendo agora, o pedido não tem pra onde ir — é
        // recusado na hora em vez de fingir sucesso. Com o módulo presente,
        // este ack só confirma "repassei pro módulo": o resultado real
        // (grãos pedidos × confirmados) chega depois no bloco `feeder` da
        // telemetria, mesmo desenho de CMD_DEVICE_DIAGNOSE.
        if (feeder_link_get_state().connected) {
          feeder_link_request_feed(cmd.feeder_grains, cmd.feeder_force);
          if (cmd.feeder_force) {
            event_log(SEV_WARN, COMP_FEEDER, "feeder.limit_overridden",
                      "Limite de refeicoes em 24 h ignorado pelo app; alimentando mesmo assim");
          }
          app_state_push_ack(cmd.id, true, nullptr);
        } else {
          app_state_push_ack(cmd.id, false, "feeder.module_offline");
        }
        break;

      case CMD_FEEDER_SET_CONFIG:
        if (feeder_link_get_state().connected) {
          feeder_link_request_config(cmd.feeder_hour1, cmd.feeder_hour2,
                                      cmd.feeder_grains, cmd.feeder_auto_enabled);
          app_state_push_ack(cmd.id, true, nullptr);
        } else {
          app_state_push_ack(cmd.id, false, "feeder.module_offline");
        }
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
  s.wifi_rssi_valid = wifi_rssi_valid();
  s.wifi_reconnects = wifi_reconnect_count();
  strncpy(s.wifi_ip, wifi_local_ip().c_str(), sizeof(s.wifi_ip) - 1);
  s.wifi_ip[sizeof(s.wifi_ip) - 1] = '\0';

  s.uptime_ms = millis();

  // Diagnóstico do controlador que nasce no núcleo de controle (UPGRADE/03,
  // F7) — o resto (heap, motivo do reset, latência do POST) é medido
  // diretamente pela task de rede, que já roda no núcleo certo para isso.
  s.button_pressed  = light_button_pressed();
  s.pot_raw_adc     = (uint16_t)fan_get_pot_raw_adc();
  s.tach_pulses_raw = fan_get_tach_pulses_raw();

  FeederLinkState fs = feeder_link_get_state();
  s.feeder_connected            = fs.connected;
  s.feeder_auto_enabled         = fs.auto_enabled;
  s.feeder_hour1                = fs.hour1;
  s.feeder_hour2                = fs.hour2;
  s.feeder_grains_per_feeding   = fs.grains_per_feeding;
  s.feeder_last_feed_age_s      = fs.last_feed_age_s;
  s.feeder_last_feed_requested  = fs.last_feed_requested;
  s.feeder_last_feed_confirmed  = fs.last_feed_confirmed;
  s.feeder_last_feed_ok         = fs.last_feed_ok;
  s.feeder_meals_24h            = fs.meals_24h;

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
  diagnostics_init();

  light_init();
  temperature_init();
  fan_init();
  rtc_init();
  feeder_link_init();

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

  // Põe o próprio loop sob o watchdog de tarefas. Até aqui só a task de rede
  // era vigiada: se o controle local travasse (um barramento preso, um
  // deadlock), luz, ventoinha e botão congelariam em silêncio — e a telemetria
  // seguiria saindo com o último retrato publicado, como se nada houvesse.
  // O prazo é o mesmo da task de rede (60 s, ver `net_task.cpp`), folga larga
  // para uma volta que normalmente leva milissegundos e, no pior caso (o
  // autodiagnóstico), menos de 1 s. O core alimenta o watchdog antes de cada
  // `loop()`; só precisamos inscrever a task.
  enableLoopWDT();
}

void loop() {
  uint32_t inicio = millis();

  _drain_commands();

  light_check_button();    // pressões já filtradas pelo amostrador de 2 ms
  temperature_update();    // conversão não-bloqueante do DS18B20
  fan_update();            // pot, histerese, cooldown, tacômetro
  rtc_check_automation();  // age só na transição de período
  feeder_link_update();    // presença no fio, UART2, PING só com o módulo presente

  _publish_snapshot();

  // Ferramentas do firmware de debug: autodiagnóstico sob demanda pelo
  // `/debug` e a marca d'água da pilha desta task (8 KB do core).
  if (dbg_diag_request) {
    dbg_diag_request = false;
    diagnostics_run(0);
  }
  dbg_loop_stack_hwm = uxTaskGetStackHighWaterMark(nullptr);
  if (dbg_hang_loop) {
    // Trava só esta task — `delay()` cede o núcleo, o resto do sistema segue.
    // Sem alimentar o watchdog, ele tem de reiniciar o chip em até 60 s.
    event_log(SEV_WARN, COMP_SYSTEM, "debug.hang", "Loop travado de proposito");
    for (;;) delay(1000);
  }

  // Período fixo em vez de `delay(200)` cru: o trabalho acima leva tempo
  // variável, e sem descontá-lo o intervalo real derivaria.
  uint32_t gasto = millis() - inicio;
  if (gasto < LOOP_PERIOD_MS) delay(LOOP_PERIOD_MS - gasto);
}
