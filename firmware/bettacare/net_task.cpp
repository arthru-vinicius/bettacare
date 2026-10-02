#include "net_task.h"

#include <esp_task_wdt.h>

#include "api_client.h"
#include "app_state.h"
#include "config.h"
#include "device_config.h"
#include "event_log.h"
#include "rtc_manager.h"
#include "web_server.h"
#include "wifi_manager.h"

/**
 * 16 KB de pilha. O caminho mais fundo é
 * `HTTPClient::POST` → `WiFiClient` → LWIP, mais a serialização do ArduinoJson
 * — e, desde o diagnóstico do controlador (UPGRADE/03, F7), `_build_body()`
 * também empilha um `DiagnosticReport` inteiro (~1,6 KB, 12 `DiagnosticCheck`)
 * em cima do `LogEvent eventos[24]` (~4 KB) que já existia, tudo na mesma
 * moldura porque o compilador inlina `_build_body()` em `api_client_post()`.
 * Com 8 KB isso corrompia a pilha bem no meio do primeiro POST depois de
 * conectar — silencioso até o heap explodir num `tlsf_walk_pool` alguns
 * ciclos depois, o pior jeito de descobrir.
 */
static const uint32_t NET_TASK_STACK = 16384;
/** Abaixo do loop principal: controle local tem prioridade sobre telemetria. */
static const UBaseType_t NET_TASK_PRIORITY = 2;
static const BaseType_t  NET_TASK_CORE = 0;

/**
 * Prazo do watchdog desta task (UPGRADE/03, F11). Generoso de propósito: cobre
 * o timeout de 4 s do HTTP com folga larga, então só dispara se a task
 * realmente travar — dentro do `HTTPClient`, por exemplo, um cenário que o
 * compilador não tem como provar impossível. Sem isto, uma trava silenciosa
 * fazia o servidor marcar o dispositivo como offline enquanto ele seguia
 * controlando o aquário perfeitamente, só sem telemetria — um diagnóstico
 * enganoso ("caiu") para o que de fato aconteceu ("uma task travou").
 */
static const uint32_t NET_TASK_WDT_TIMEOUT_S = 60;

/** Teto de segurança do reboot pedido pelo servidor (ver `app_state_request_reboot`). */
static const uint32_t REBOOT_SAFETY_TIMEOUT_MS = 60000;

/** Ressincroniza mesmo sem perda de energia — mantém o relógio honesto no longo prazo. */
static const uint32_t NTP_RESYNC_INTERVAL_MS = 24UL * 60 * 60 * 1000;

static bool _ever_connected = false;

bool net_task_ever_connected() { return _ever_connected; }

static void _init_task_watchdog() {
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms     = NET_TASK_WDT_TIMEOUT_S * 1000UL;
  cfg.idle_core_mask = (1 << 0);  // mantém a vigilância do IDLE0 que o core já faz
  cfg.trigger_panic  = true;

  // O core do Arduino-ESP32 já inicia o TWDT no boot (5 s, com reinício —
  // CONFIG_ESP_TASK_WDT_INIT/PANIC). Aí `esp_task_wdt_init()` falha com
  // ESP_ERR_INVALID_STATE (o "TWDT already initialized" do log de boot), e a
  // versão anterior só ignorava o erro e inscrevia a task: o prazo de 60 s
  // nunca valia, ela ficava vigiada com 5 s. Um NTP lento já bastava para
  // reiniciar o chip, e sem internet virava loop de reboot (UPGRADE/07).
  esp_err_t err = esp_task_wdt_init(&cfg);
  if (err == ESP_ERR_INVALID_STATE) err = esp_task_wdt_reconfigure(&cfg);
  if (err != ESP_OK) {
    event_log(SEV_ERROR, COMP_SYSTEM, "system.wdt_config_failed",
              "Watchdog da task de rede nao configurado (erro %d)", (int)err);
  }
  esp_task_wdt_add(nullptr);
}

static void _net_loop(void*) {
  _init_task_watchdog();

  uint32_t proximo_post_ms = 0;
  /** `app_state_ack_generation()` do último POST — ver o envio antecipado abaixo. */
  uint32_t acks_ja_enviados = 0;
  uint32_t ultima_tentativa_ntp = 0;
  /** 0 = nunca sincronizado — força a primeira tentativa assim que houver rede. */
  uint32_t ultimo_ntp_ok_ms = 0;

  for (;;) {
    esp_task_wdt_reset();

    wifi_check_reconnect();
    webserver_loop();

    /**
     * NTP: primeira sincronização, ressincronização diária, e imediatamente
     * se o RTC perder a hora (UPGRADE/03, F3).
     *
     * Antes disto, `_ntp_synced` virava `true` no primeiro sucesso e nunca
     * mais voltava a `false`. Enquanto o DS3231 está bem isso não incomoda —
     * mas se a bateria acabasse em operação, a hora ficava errada **para
     * sempre**, porque a única função que a corrige nunca mais era chamada.
     * Uma hora errada quebra a automação inteira da luminária.
     */
    if (wifi_is_connected()) {
      uint32_t agora_ntp = millis();
      bool devido = ultimo_ntp_ok_ms == 0 || rtc_lost_power() ||
                    (agora_ntp - ultimo_ntp_ok_ms) >= NTP_RESYNC_INTERVAL_MS;

      if (devido &&
          (ultima_tentativa_ntp == 0 || agora_ntp - ultima_tentativa_ntp >= 60000UL)) {
        ultima_tentativa_ntp = agora_ntp;
        if (rtc_sync_ntp()) ultimo_ntp_ok_ms = agora_ntp;
      }
    }

    uint32_t agora = millis();

    /**
     * Confirmação na hora (UPGRADE/07). Um comando executado esperava o
     * próximo ciclo para ter o `ack` enviado — até um intervalo inteiro a mais
     * entre o toque no app e o "confirmado". Agora uma confirmação nova
     * antecipa o POST, que leva junto o estado já mudado.
     *
     * No máximo um POST antecipado por leva de confirmações, e nunca com o
     * servidor falhando: aí vale o recuo progressivo, senão um servidor fora
     * do ar seria martelado a cada volta desta task. Um 400 também zera as
     * falhas mas deixa o `ack` na fila — por isso a marca de "já enviado" é a
     * geração, não a fila estar vazia.
     */
    uint32_t geracao_acks = app_state_ack_generation();
    bool ack_novo = geracao_acks != acks_ja_enviados && api_client_consecutive_failures() == 0;

    if (ack_novo || (int32_t)(agora - proximo_post_ms) >= 0) {
      acks_ja_enviados = geracao_acks;
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

      /**
       * Reboot pedido pelo servidor (UPGRADE/03, F2): só reinicia depois de um
       * POST bem-sucedido — é ele que leva o `ack` do `device.reboot`, que só
       * sai do buffer após a confirmação de envio (ver F1 em `api_client.cpp`).
       * Reiniciar antes fazia o comando aparecer como "expirado" quase sempre,
       * mesmo tendo funcionado.
       */
      if (r == API_OK && app_state_reboot_requested()) {
        delay(50);
        ESP.restart();
      }
    }

    // Teto de segurança: se a rede não voltar, reinicia de qualquer forma em
    // vez de ficar preso esperando um POST que talvez nunca aconteça.
    if (app_state_reboot_requested() &&
        millis() - app_state_reboot_requested_ms() >= REBOOT_SAFETY_TIMEOUT_MS) {
      ESP.restart();
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
