#include "api_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <math.h>

#include "app_state.h"
#include "config.h"
#include "device_config.h"
#include "event_log.h"

#define BOOT_NS      "bc_boot"
#define BOOT_KEY_ID  "boot_id"

static uint32_t _boot_id = 0;
static uint32_t _seq     = 0;
static uint16_t _failures = 0;
static uint32_t _last_success_ms = 0;
static bool     _boot_id_loaded = false;

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
  wifi["rssi"] = s.wifi_rssi;
  if (s.wifi_ip[0]) {
    wifi["ip"] = s.wifi_ip;
  } else {
    wifi["ip"] = nullptr;
  }
  wifi["reconnects"] = s.wifi_reconnects;

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
}

// ── Interpretação da resposta ───────────────────────────────────────────────

static CommandKind _kind_from(const char* action) {
  if (strcmp(action, "light.set")     == 0) return CMD_LIGHT_SET;
  if (strcmp(action, "fan.set_speed") == 0) return CMD_FAN_SET_SPEED;
  if (strcmp(action, "fan.set_mode")  == 0) return CMD_FAN_SET_MODE;
  if (strcmp(action, "config.apply")  == 0) return CMD_CONFIG_APPLY;
  if (strcmp(action, "device.reboot") == 0) return CMD_DEVICE_REBOOT;
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
  uint8_t n_eventos = event_log_drain(eventos, EVENT_BUFFER_SIZE);

  CommandAck acks[8];
  uint8_t n_acks = app_state_drain_acks(acks, 8);

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

  int status = http.POST(corpo);
  ApiResult resultado;

  if (status == 200) {
    JsonDocument resp;
    DeserializationError err = deserializeJson(resp, http.getStream());
    if (err) {
      resultado = API_BAD_RESPONSE;
    } else {
      _handle_response(resp);
      _seq++;                       // só avança quando a troca deu certo
      _failures = 0;
      _last_success_ms = millis();
      resultado = API_OK;
    }
  } else if (status == 401) {
    resultado = API_UNAUTHORIZED;
  } else if (status >= 500) {
    resultado = API_SERVER_ERROR;
  } else {
    resultado = API_HTTP_ERROR;
  }

  http.end();

  if (resultado != API_OK) {
    _failures++;
    /**
     * Os eventos drenados voltam para o buffer? Não — e é deliberado. Eles já
     * foram impressos no Serial, e reinserir criaria um laço: falha de rede
     * gera evento, que engorda o próximo corpo, que tem mais chance de falhar.
     * O que não se perde é o **estado**, que é reenviado inteiro no POST
     * seguinte. Log é diagnóstico; estado é o que o aquário precisa.
     */
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
