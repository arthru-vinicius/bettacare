#include "net_task.h"

#include "api_client.h"
#include "config.h"
#include "device_config.h"
#include "event_log.h"
#include "rtc_manager.h"
#include "web_server.h"
#include "wifi_manager.h"

/**
 * 8 KB de pilha. O caminho mais fundo é
 * `HTTPClient::POST` → `WiFiClient` → LWIP, mais a serialização do ArduinoJson.
 * Com 4 KB o stack overflow aparece só sob carga, que é o pior jeito de
 * descobrir.
 */
static const uint32_t NET_TASK_STACK = 8192;
/** Abaixo do loop principal: controle local tem prioridade sobre telemetria. */
static const UBaseType_t NET_TASK_PRIORITY = 2;
static const BaseType_t  NET_TASK_CORE = 0;

static bool _ever_connected = false;
static bool _ntp_synced = false;

bool net_task_ever_connected() { return _ever_connected; }

static void _net_loop(void*) {
  uint32_t proximo_post_ms = 0;
  uint32_t ultima_tentativa_ntp = 0;

  for (;;) {
    wifi_check_reconnect();
    webserver_loop();

    // NTP só depois de haver rede, e sem impedir o resto de rodar.
    if (!_ntp_synced && wifi_is_connected()) {
      uint32_t agora = millis();
      if (ultima_tentativa_ntp == 0 || agora - ultima_tentativa_ntp >= 60000UL) {
        ultima_tentativa_ntp = agora;
        _ntp_synced = rtc_sync_ntp();
      }
    }

    uint32_t agora = millis();
    if ((int32_t)(agora - proximo_post_ms) >= 0) {
      ApiResult r = api_client_post();

      if (r == API_OK && !_ever_connected) {
        _ever_connected = true;
        event_log(SEV_INFO, COMP_API, "api.connected",
                  "Primeiro contato com o servidor estabelecido");
      }

      /**
       * Recuo progressivo quando o servidor não responde.
       *
       * Sem isso, um servidor fora do ar renderia 20 requisições por minuto
       * sem propósito, cada uma com timeout de 4 s, consumindo rádio e
       * bateria do roteador à toa. Com recuo, o aquário simplesmente segue
       * operando sozinho e volta a falar quando houver com quem.
       *
       * O teto é 30 s: adianta pouco esperar mais, e um teto baixo faz a
       * telemetria voltar rápido quando o servidor enfim sobe.
       */
      uint32_t intervalo = device_config_snapshot().telemetry_interval_ms;
      uint16_t falhas = api_client_consecutive_failures();
      if (falhas > 0) {
        uint32_t recuo = intervalo * (falhas < 5 ? falhas : 5);
        if (recuo > 30000UL) recuo = 30000UL;
        intervalo = recuo;
      }

      proximo_post_ms = agora + intervalo;
    }

    // Cede o núcleo. 50 ms mantém o OTA responsivo sem girar em vão.
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void net_task_start() {
  BaseType_t ok = xTaskCreatePinnedToCore(_net_loop, "bc_net", NET_TASK_STACK,
                                          nullptr, NET_TASK_PRIORITY, nullptr,
                                          NET_TASK_CORE);
  if (ok != pdPASS) {
    // Sem a task de rede o aquário continua funcionando sozinho — luz por
    // horário, ventoinha por temperatura, botão físico. Só fica sem telemetria.
    event_log(SEV_FATAL, COMP_SYSTEM, "system.task_failed",
              "Nao foi possivel criar a task de rede; operando apenas local");
  }
}
