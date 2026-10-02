#include "api_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <math.h>

#include "app_state.h"
#include "config.h"
#include "device_config.h"
#include "diagnostics.h"
#include "event_log.h"
#include "feeder_link.h"
#include "web_server.h"

#define BOOT_NS      "bc_boot"
#define BOOT_KEY_ID  "boot_id"

static uint32_t _boot_id = 0;
static uint32_t _seq     = 0;
static uint16_t _failures = 0;
static uint32_t _last_success_ms = 0;
static bool     _boot_id_loaded = false;

/** Latência do POST anterior — não dá para medir a deste antes de enviá-lo. */
static uint32_t _last_post_latency_ms   = 0;
static bool     _has_post_latency       = false;

/** Código HTTP da última resposta (negativo = erro de conexão do HTTPClient; 0 = nenhuma ainda). */
static volatile int _last_http_status = 0;

/**
 * Contador de boots, persistido em NVS.
 *
 * Junto com o `seq`, é o que dá idempotência ao servidor: um `boot_id` novo
 * avisa que o `seq` recomeçou do zero, e sem ele um reinício do ESP32 pareceria
 * uma enxurrada de reenvios antigos.
 */
static void _load_boot_id() {
  if (_boot_id_loaded) return;
  _boot_id_loaded = true;

  Preferences prefs;
  if (!prefs.begin(BOOT_NS, false)) {
    // Sem NVS, usa um valor derivado do relógio de boot. Não é monotônico
    // entre reinícios, mas evita colidir com o boot anterior no caso comum.
    _boot_id = (uint32_t)(esp_random() & 0x7FFFFFFF);
    nvs_report_failure();
    event_log(SEV_WARN, COMP_NVS, "nvs.boot_id_failed",
              "boot_id nao persistido; usando valor aleatorio");
    return;
  }

  _boot_id = prefs.getUInt(BOOT_KEY_ID, 0) + 1;
  prefs.putUInt(BOOT_KEY_ID, _boot_id);
  prefs.end();
}

void api_client_init() {
  _load_boot_id();
  _seq = 0;
  _failures = 0;
  _last_success_ms = 0;
  event_log(SEV_INFO, COMP_SYSTEM, "system.boot",
            "Boot #%lu, firmware %s", (unsigned long)_boot_id, FW_VERSION);
}

uint16_t api_client_consecutive_failures() { return _failures; }
uint32_t api_client_last_success_ms() { return _last_success_ms; }
int api_client_last_http_status() { return _last_http_status; }

// ── Diagnóstico do controlador (UPGRADE/03, F7) ─────────────────────────────

/** Nomes curtos e estáveis — o que `diagSchema.reset_reason` espera. */
static const char* _reset_reason_name() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:    return "poweron";
    case ESP_RST_EXT:        return "ext";
    case ESP_RST_SW:         return "sw";
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "int_wdt";
    case ESP_RST_TASK_WDT:   return "task_wdt";
    case ESP_RST_WDT:        return "wdt";
    case ESP_RST_DEEPSLEEP:  return "deepsleep";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_SDIO:       return "sdio";
    case ESP_RST_USB:        return "usb";
    case ESP_RST_JTAG:       return "jtag";
    case ESP_RST_EFUSE:      return "efuse";
    case ESP_RST_PWR_GLITCH: return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default:                 return "unknown";
  }
}

// ── Montagem do corpo ───────────────────────────────────────────────────────

static void _build_body(JsonDocument& doc, const DeviceSnapshot& s,
                        LogEvent* eventos, uint8_t n_eventos,
                        CommandAck* acks, uint8_t n_acks) {
  doc["device_id"]      = DEVICE_ID;
  doc["boot_id"]        = _boot_id;
  doc["seq"]            = _seq;
  doc["uptime_ms"]      = s.uptime_ms;
  doc["config_version"] = device_config_version();
  doc["fw_version"]     = FW_VERSION;

  JsonObject light = doc["light"].to<JsonObject>();
  light["on"]     = s.light_on;
  light["source"] = light_source_name(s.light_source);

  JsonObject temp = doc["temperature"].to<JsonObject>();
  // `null` explícito, nunca uma sentinela como 0 ou -127: o contrato distingue
  // "sensor ausente" de "está zero grau", e a diferença importa.
  if (s.temp_valid && !isnan(s.temp_celsius)) {
    temp["celsius"] = serialized(String(s.temp_celsius, 2));
  } else {
    temp["celsius"] = nullptr;
  }
  temp["available"] = s.temp_available;
  temp["valid"]     = s.temp_valid;
  // `UINT32_MAX` e a sentinela de "nunca houve leitura valida", e ela nao pode
  // viajar como numero: nao cabe num `integer` do PostgreSQL e derrubava o
  // ingest com 500 a cada POST. Ausencia se representa com `null`.
  if (s.temp_age_ms == UINT32_MAX) {
    temp["age_ms"] = nullptr;
  } else {
    temp["age_ms"] = s.temp_age_ms;
  }

  JsonObject fan = doc["fan"].to<JsonObject>();
  fan["on"]            = s.fan_on;
  fan["speed_percent"] = s.fan_speed_percent;
  fan["rpm"]           = s.fan_rpm;
  fan["mode"]          = fan_mode_name(s.fan_mode);

  JsonObject rtc = doc["rtc"].to<JsonObject>();
  rtc["available"]  = s.rtc_available;
  if (s.rtc_available) {
    rtc["time"] = s.rtc_time;
  } else {
    rtc["time"] = nullptr;
  }
  rtc["lost_power"] = s.rtc_lost_power;

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  // `-120` é valor de repouso, não medição — sem a checagem de validade, um
  // snapshot capturado durante uma queda breve viajava como se fosse RSSI
  // real (UPGRADE/03, F13, e a mesma família do bug de sentinela numérica
  // corrigido em `a4596fe`).
  if (s.wifi_rssi_valid) {
    wifi["rssi"] = s.wifi_rssi;
  } else {
    wifi["rssi"] = nullptr;
  }
  if (s.wifi_ip[0]) {
    wifi["ip"] = s.wifi_ip;
  } else {
    wifi["ip"] = nullptr;
  }
  wifi["reconnects"] = s.wifi_reconnects;

  /**
   * Módulo opcional de alimentação: o bloco só vai com ele conectado — linha
   * válida pelo fio nos últimos 6 s, ver `feeder_link.h`. Sem o bloco, o
   * servidor entende "desconectado" e guarda a última agenda conhecida;
   * desconectado é o estado normal de um módulo que não fica ligado o tempo
   * todo, nunca uma falha.
   *
   * Até a 2.0.0 o bloco ia inteiro em todo POST, zerado quando o módulo nunca
   * tinha respondido: `grains_per_feeding: 0` fica abaixo do mínimo e virava
   * um evento `ingest.field_rejected` por POST em produção, e a agenda 00h/00h
   * aparecia no app para um módulo que nunca existiu.
   */
  if (s.feeder_connected) {
    JsonObject feeder = doc["feeder"].to<JsonObject>();
    feeder["connected"] = true;
    // A agenda, só depois do primeiro PONG/SCHEDULE válido. `connected` vira
    // verdadeiro com qualquer linha reconhecida, até antes disso, e uma agenda
    // de verdade nunca tem 0 grãos (`_parse_schedule` exige 1 a 20).
    if (s.feeder_grains_per_feeding > 0) {
      feeder["auto_enabled"]       = s.feeder_auto_enabled;
      feeder["hour1"]              = s.feeder_hour1;
      feeder["hour2"]              = s.feeder_hour2;
      feeder["grains_per_feeding"] = s.feeder_grains_per_feeding;
    }
    // A última alimentação, só se houve uma (`UINT32_MAX` é "nunca alimentou").
    if (s.feeder_last_feed_age_s != UINT32_MAX) {
      feeder["last_feed_age_s"]     = s.feeder_last_feed_age_s;
      feeder["last_feed_requested"] = s.feeder_last_feed_requested;
      feeder["last_feed_confirmed"] = s.feeder_last_feed_confirmed;
      feeder["last_feed_ok"]        = s.feeder_last_feed_ok;
    }
    if (s.feeder_meals_24h != FEEDER_MEALS_UNKNOWN) {
      feeder["meals_24h"] = s.feeder_meals_24h;
    }
  }

  if (n_acks > 0) {
    JsonArray arr = doc["ack"].to<JsonArray>();
    for (uint8_t i = 0; i < n_acks; i++) {
      JsonObject a = arr.add<JsonObject>();
      a["id"] = acks[i].id;
      a["ok"] = acks[i].ok;
      // O contrato antigo previa `ack: [91]` — uma lista de números, que só
      // sabe dizer "recebi" e nunca "nao consegui". Com o objeto, a falha volta
      // com nome, e é dela que sai a mensagem que o usuario le na interface.
      if (!acks[i].ok && acks[i].code[0]) a["code"] = acks[i].code;
    }
  }

  if (n_eventos > 0) {
    JsonArray arr = doc["events"].to<JsonArray>();
    for (uint8_t i = 0; i < n_eventos; i++) {
      JsonObject e = arr.add<JsonObject>();
      e["sev"]  = event_severity_name(eventos[i].sev);
      e["comp"] = event_component_name(eventos[i].comp);
      e["code"] = eventos[i].code;
      e["msg"]  = eventos[i].msg;
      if (eventos[i].repeat_count > 1) e["repeat_count"] = eventos[i].repeat_count;

      /**
       * A hora local do RTC vai no `ctx`, não no campo `t` do contrato: `t`
       * espera um instante ISO completo e aqui só existe "HH:MM" — o RTC não
       * guarda fuso e a data sozinha não bastaria.
       *
       * Importa no caso em que o dispositivo passou horas sem rede: os eventos
       * ficam represados e chegam todos com o mesmo `received_at`, então sem
       * isto não haveria como saber quando cada um realmente aconteceu.
       */
      if (eventos[i].time[0] && strcmp(eventos[i].time, "--:--") != 0) {
        e["ctx"]["device_time"] = eventos[i].time;
      }
    }
  }

  /**
   * Diagnóstico do controlador (UPGRADE/03, F7) — responde ao que era
   * impossível responder antes: o ESP32 reiniciou esta noite, e por quê; a
   * memória está caindo ao longo dos dias; o potenciômetro tem mau contato.
   *
   * Heap, motivo do reset e a pilha da própria task são medidos aqui, não no
   * núcleo de controle: `_build_body` já roda na task de rede, e essas
   * grandezas são globais ao chip — medi-las por qualquer núcleo dá a mesma
   * resposta, então não há razão para rotear pelo snapshot mutex-protegido.
   */
  JsonObject diag = doc["diag"].to<JsonObject>();
  diag["reset_reason"]       = _reset_reason_name();
  diag["free_heap"]          = (uint32_t)esp_get_free_heap_size();
  diag["min_free_heap"]      = (uint32_t)esp_get_minimum_free_heap_size();
  diag["max_alloc_heap"]     = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  diag["api_failures"]       = _failures;
  diag["pot_raw_adc"]        = s.pot_raw_adc;
  diag["button_pressed"]     = s.button_pressed;
  diag["tach_pulses_raw"]    = s.tach_pulses_raw;
  diag["net_task_stack_hwm"] = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
  diag["nvs_failures"]       = nvs_failure_count();
  diag["ota_last_result"]    = webserver_ota_last_result();
  diag["events_dropped"]     = event_log_dropped_count();
  if (_has_post_latency) {
    diag["post_latency_ms"] = _last_post_latency_ms;
  } else {
    diag["post_latency_ms"] = nullptr;
  }

  /**
   * Relatório de autodiagnóstico, quando houver um esperando.
   *
   * Viaja uma vez só: `diagnostics_take()` o marca como consumido. Se este
   * POST falhar o relatório se perde — e isso é aceitável de um jeito que a
   * perda de evento não era, porque um autodiagnóstico é reprodutível: basta
   * pedir de novo pela interface, e a resposta será mais atual que a anterior.
   */
  DiagnosticReport rel;
  if (diagnostics_take(rel) && rel.count > 0) {
    JsonObject d = doc["diagnostic"].to<JsonObject>();
    if (rel.command_id > 0) d["command_id"] = rel.command_id;
    d["ran_at_uptime_ms"] = rel.ran_at_uptime_ms;
    d["duration_ms"]      = rel.duration_ms;

    JsonArray checks = d["checks"].to<JsonArray>();
    for (uint8_t i = 0; i < rel.count; i++) {
      JsonObject c = checks.add<JsonObject>();
      c["comp"]   = event_component_name(rel.checks[i].comp);
      c["status"] = rel.checks[i].status;
      c["detail"] = rel.checks[i].detail;
      c["probed"] = rel.checks[i].probed;
    }
  }
}

// ── Interpretação da resposta ───────────────────────────────────────────────

static CommandKind _kind_from(const char* action) {
  if (strcmp(action, "light.set")     == 0) return CMD_LIGHT_SET;
  if (strcmp(action, "fan.set_speed") == 0) return CMD_FAN_SET_SPEED;
  if (strcmp(action, "fan.set_mode")  == 0) return CMD_FAN_SET_MODE;
  if (strcmp(action, "config.apply")  == 0) return CMD_CONFIG_APPLY;
  if (strcmp(action, "device.reboot") == 0) return CMD_DEVICE_REBOOT;
  if (strcmp(action, "device.diagnose") == 0) return CMD_DEVICE_DIAGNOSE;
  if (strcmp(action, "feeder.feed_now")  == 0) return CMD_FEEDER_FEED_NOW;
  if (strcmp(action, "feeder.set_config") == 0) return CMD_FEEDER_SET_CONFIG;
  return CMD_UNKNOWN;
}

static bool _parse_hhmm(const char* s, uint8_t& h, uint8_t& m) {
  if (s == nullptr || strlen(s) != 5 || s[2] != ':') return false;
  h = (uint8_t)((s[0] - '0') * 10 + (s[1] - '0'));
  m = (uint8_t)((s[3] - '0') * 10 + (s[4] - '0'));
  return h <= 23 && m <= 59;
}

static void _handle_response(JsonDocument& doc) {
  uint32_t server_version = doc["config_version"] | 0;

  // O bloco `config` só viaja quando o servidor nota que estamos atrasados.
  JsonObjectConst cfg = doc["config"];
  if (!cfg.isNull()) {
    DeviceConfig nova = device_config_snapshot();
    bool ok = true;

    ok &= _parse_hhmm(cfg["light_on_time"]  | (const char*)nullptr,
                      nova.light_on_hour, nova.light_on_min);
    ok &= _parse_hhmm(cfg["light_off_time"] | (const char*)nullptr,
                      nova.light_off_hour, nova.light_off_min);

    nova.fan_trigger_c = cfg["fan_trigger_c"] | nova.fan_trigger_c;
    nova.fan_off_c     = cfg["fan_off_c"]     | nova.fan_off_c;
    nova.telemetry_interval_ms =
        cfg["telemetry_interval_ms"] | nova.telemetry_interval_ms;
    nova.heartbeat_interval_ms =
        cfg["heartbeat_interval_ms"] | nova.heartbeat_interval_ms;

    if (ok) {
      device_config_apply(nova, server_version);
    } else {
      event_log(SEV_ERROR, COMP_API, "api.bad_config",
                "Horarios da config vieram malformados; mantendo a anterior");
    }
  }

  JsonArrayConst cmds = doc["commands"];
  for (JsonObjectConst c : cmds) {
    PendingCommand cmd = {};
    cmd.id = c["id"] | 0;
    const char* action = c["action"] | "";
    cmd.kind = _kind_from(action);

    if (cmd.id == 0) continue;

    if (cmd.kind == CMD_UNKNOWN) {
      // Um firmware antigo diante de um comando novo. Recusar com código
      // explícito é melhor que ignorar: o comando sai da fila do servidor e o
      // usuário vê o motivo, em vez de ficar esperando para sempre.
      app_state_push_ack(cmd.id, false, "cmd.unsupported");
      event_log(SEV_WARN, COMP_API, "api.unknown_command",
                "Comando desconhecido recusado: %s", action);
      continue;
    }

    cmd.on        = c["on"]      | false;
    cmd.percent   = c["percent"] | 0;
    const char* mode = c["mode"] | "auto";
    cmd.mode_auto = (strcmp(mode, "auto") == 0);

    // CMD_FEEDER_FEED_NOW só usa `feeder_grains` (0 = padrão do módulo).
    // CMD_FEEDER_SET_CONFIG usa os quatro campos — sempre juntos, nunca um
    // isolado (ver o comentário em `feeder.set_config` no contrato).
    cmd.feeder_grains = (uint8_t)(cmd.kind == CMD_FEEDER_SET_CONFIG
                                      ? (c["grains_per_feeding"] | 0)
                                      : (c["grains"] | 0));
    cmd.feeder_hour1        = (uint8_t)(c["hour1"] | 0);
    cmd.feeder_hour2        = (uint8_t)(c["hour2"] | 0);
    cmd.feeder_auto_enabled = c["auto_enabled"] | true;
    cmd.feeder_force        = c["force"] | false;

    if (!app_state_push_command(cmd)) {
      app_state_push_ack(cmd.id, false, "cmd.queue_full");
      event_log(SEV_ERROR, COMP_API, "api.queue_full",
                "Fila de comandos cheia; #%lu recusado", (unsigned long)cmd.id);
    }
  }
}

// ── O POST ──────────────────────────────────────────────────────────────────

ApiResult api_client_post() {
  if (WiFi.status() != WL_CONNECTED) return API_NO_WIFI;

  DeviceSnapshot s;
  if (!app_state_snapshot(s)) return API_HTTP_ERROR;  // loop ainda não publicou

  // Sinaliza perda antes de drenar, para o aviso viajar junto com o lote em
  // que a perda aconteceu.
  if (event_log_take_overflow_flag()) {
    event_log(SEV_WARN, COMP_SYSTEM, "system.event_overflow",
              "Eventos descartados: buffer cheio entre dois envios");
  }

  LogEvent eventos[EVENT_BUFFER_SIZE];
  uint8_t n_eventos = event_log_peek(eventos, EVENT_BUFFER_SIZE);

  // `peek`, não drena: se o POST falhar, os acks continuam na fila para o
  // próximo ciclo tentar de novo (UPGRADE/03, F1). Só são removidos de fato
  // depois da confirmação do servidor, mais abaixo.
  CommandAck acks[8];
  uint8_t n_acks = app_state_peek_acks(acks, 8);

  JsonDocument doc;
  _build_body(doc, s, eventos, n_eventos, acks, n_acks);

  String corpo;
  serializeJson(doc, corpo);

  char url[128];
  snprintf(url, sizeof(url), "http://%s:%d%s", SERVER_HOST, SERVER_PORT,
           SERVER_TELEMETRY_PATH);

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);

  if (!http.begin(client, url)) {
    _failures++;
    return API_HTTP_ERROR;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Api-Token", SERVER_API_TOKEN);

  uint32_t inicio_post = millis();
  int status = http.POST(corpo);
  _last_post_latency_ms = millis() - inicio_post;
  _has_post_latency = true;
  _last_http_status = status;

  ApiResult resultado;

  if (status == 200) {
    JsonDocument resp;
    DeserializationError err = deserializeJson(resp, http.getStream());
    if (err) {
      resultado = API_BAD_RESPONSE;
    } else {
      _handle_response(resp);
      app_state_confirm_acks_sent(n_acks);  // só agora saem de fato da fila
      event_log_confirm_sent(n_eventos);
      _seq++;                       // só avança quando a troca deu certo
      _failures = 0;
      _last_success_ms = millis();
      resultado = API_OK;
    }
  } else if (status == 400) {
    /**
     * O servidor recusou o corpo mesmo depois de tentar corrigi-lo sozinho
     * (UPGRADE/05, C1). Repetir o mesmo corpo daria o mesmo resultado — ao
     * contrário das outras falhas, o `seq` avança mesmo sem sucesso pleno,
     * para o próximo ciclo enviar um corpo novo (com leituras frescas) em vez
     * de insistir indefinidamente no que acabou de ser rejeitado. Os `ack`
     * continuam na fila: o servidor não os processou, então não são
     * confirmados como enviados.
     */
    // Os eventos saem do buffer mesmo assim: o corpo que os levava foi
    // rejeitado por um campo fora de faixa, não pelos eventos, e reenviá-los
    // no mesmo corpo envenenado só repetiria a rejeição. Os graves ficam.
    event_log_release_sent(n_eventos);
    event_log(SEV_WARN, COMP_API, "api.rejected_body",
              "Servidor recusou o corpo (HTTP 400); seguindo com o proximo ciclo");
    _seq++;
    _failures = 0;
    // `_last_success_ms` NÃO avança (UPGRADE/07): o servidor estava de pé, mas
    // não aceitou nada. Marcar como sucesso fazia o `/status` local dizer
    // "tudo certo, contato há 1 s" enquanto o painel dizia "sem contato há 20
    // minutos" — e o motivo real ficava invisível dos dois lados.
    resultado = API_HTTP_ERROR;
  } else if (status == 401) {
    resultado = API_UNAUTHORIZED;
  } else if (status >= 500) {
    resultado = API_SERVER_ERROR;
  } else {
    resultado = API_HTTP_ERROR;
  }

  http.end();

  if (resultado != API_OK && status != 400) {
    _failures++;
    /**
     * Os eventos que iam neste corpo voltam para o buffer? **Os graves, sim.**
     *
     * A versão anterior descartava todos, com um argumento correto pela
     * metade: reinserir tudo cria um laço — falha de rede gera evento, que
     * engorda o próximo corpo, que tem mais chance de falhar. Só que descartar
     * tudo joga fora exatamente o diagnóstico que explica a falha, e é durante
     * uma queda de rede que ele mais importa.
     *
     * `event_log_release_sent()` resolve o meio-termo: solta `debug`, `info` e
     * `warn` (o volume, que é ruído sem rede) e retém `error` e `fatal` (o
     * sinal, que é raro e não engorda nada). O **estado** continua sendo
     * reenviado inteiro no POST seguinte, como sempre foi.
     */
    event_log_release_sent(n_eventos);

    if (_failures == 1 || _failures % 20 == 0) {
      const char* motivo = resultado == API_UNAUTHORIZED ? "api.unauthorized"
                         : resultado == API_SERVER_ERROR ? "api.server_error"
                                                         : "api.post_failed";
      event_log(SEV_ERROR, COMP_API, motivo,
                "Falha ao enviar telemetria (HTTP %d), tentativa %u",
                status, (unsigned)_failures);
    }
  }

  return resultado;
}
