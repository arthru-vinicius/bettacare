#include "web_server.h"
#include "api_client.h"
#include "app_state.h"
#include "config.h"
#include "debug_probe.h"
#include "device_config.h"
#include "event_log.h"
#include "fan.h"
#include "light.h"
#include "rtc_manager.h"
#include "temperature.h"
#include "wifi_manager.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#define ELEGANTOTA_USE_ASYNC_WEBSERVER 1
#include <ElegantOTA.h>
#include <Preferences.h>
#include <WiFi.h>
#include <string.h>

static AsyncWebServer _server(80);

// Rastreia o início da última atualização OTA para log de progresso
static unsigned long _ota_progress_ms = 0;

// ── Desfecho do último OTA, persistido ───────────────────────────────────────
#define OTA_NS      "bc_ota"
#define OTA_KEY     "last"

static char _ota_last_result[8] = "none";

const char* webserver_ota_last_result() { return _ota_last_result; }

/** Lê o desfecho gravado antes do reboot que o próprio OTA provocou. */
static void _ota_load_last_result() {
  Preferences prefs;
  if (!prefs.begin(OTA_NS, true)) return;
  String v = prefs.getString(OTA_KEY, "none");
  prefs.end();

  if (v == "ok" || v == "failed") {
    strncpy(_ota_last_result, v.c_str(), sizeof(_ota_last_result) - 1);
    _ota_last_result[sizeof(_ota_last_result) - 1] = '\0';
  }
}

static void _ota_store_result(const char* resultado) {
  strncpy(_ota_last_result, resultado, sizeof(_ota_last_result) - 1);
  _ota_last_result[sizeof(_ota_last_result) - 1] = '\0';

  Preferences prefs;
  if (!prefs.begin(OTA_NS, false)) {
    nvs_report_failure();
    return;
  }
  prefs.putString(OTA_KEY, resultado);
  prefs.end();
}

#ifndef API_AUTH_TOKEN
#define API_AUTH_TOKEN ""
#endif

#ifndef CORS_ALLOWED_ORIGIN
#define CORS_ALLOWED_ORIGIN ""
#endif

static bool _cors_enabled() {
  return strlen(CORS_ALLOWED_ORIGIN) > 0;
}

static bool _is_allowed_origin(const String &origin) {
  if (!_cors_enabled()) return false;
  return origin.equals(CORS_ALLOWED_ORIGIN);
}

static void _add_cors_headers(AsyncWebServerRequest *request, AsyncWebServerResponse *response) {
  if (!request->hasHeader("Origin")) return;
  String origin = request->header("Origin");
  if (!_is_allowed_origin(origin)) return;

  response->addHeader("Access-Control-Allow-Origin", CORS_ALLOWED_ORIGIN);
  response->addHeader("Vary", "Origin");
  response->addHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  response->addHeader("Access-Control-Allow-Headers", "Content-Type, X-Api-Token");
}

static void _send_json(AsyncWebServerRequest *request, int status, const String &payload) {
  AsyncWebServerResponse *response = request->beginResponse(status, "application/json", payload);
  _add_cors_headers(request, response);
  request->send(response);
}

static bool _auth_enabled() {
  return strlen(API_AUTH_TOKEN) > 0;
}

/**
 * Compara em tempo constante (UPGRADE/03, F14) — o mesmo cuidado que o
 * servidor já tem no ingest (`ingest/auth.ts`, SHA-256 dos dois lados antes
 * do `timingSafeEqual`). Aqui é a API local do próprio ESP32, numa LAN
 * doméstica, então a severidade é baixa; mas não havia motivo para a
 * assimetria entre os dois lados do mesmo sistema.
 */
static bool _constant_time_equal(const String &a, const char *b) {
  size_t len_a = a.length();
  size_t len_b = strlen(b);
  uint8_t diff = (uint8_t)(len_a != len_b);
  size_t n = len_a < len_b ? len_a : len_b;
  for (size_t i = 0; i < n; i++) {
    diff |= (uint8_t)(a[i] ^ b[i]);
  }
  return diff == 0;
}

static bool _is_authorized(AsyncWebServerRequest *request) {
  if (!_auth_enabled()) return true;

  String provided;
  if (request->hasHeader("X-Api-Token")) {
    provided = request->header("X-Api-Token");
  } else if (request->hasParam("token")) {
    provided = request->getParam("token")->value();
  }

  return _constant_time_equal(provided, API_AUTH_TOKEN);
}

static bool _require_auth(AsyncWebServerRequest *request) {
  if (_is_authorized(request)) return true;
  _send_json(request, 401, "{\"error\":\"Unauthorized\"}");
  return false;
}

static bool _require_setup_auth(AsyncWebServerRequest *request) {
  if (request->authenticate(OTA_USERNAME, OTA_PASSWORD)) return true;
  request->requestAuthentication();
  return false;
}

static String _escape_html(const String &text) {
  String out;
  out.reserve(text.length() + 16);
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

static String _build_wifi_setup_page(const String &message, bool isError) {
  String configuredSsid = _escape_html(wifi_configured_ssid());
  String apSsid         = _escape_html(wifi_recovery_ap_ssid());
  String stationIp      = wifi_is_connected() ? WiFi.localIP().toString() : String("--");
  String apIp           = wifi_recovery_ap_active() ? WiFi.softAPIP().toString() : String("--");

  String notice = "";
  if (message.length() > 0) {
    notice = "<div class='notice ";
    notice += (isError ? "err" : "ok");
    notice += "'>";
    notice += _escape_html(message);
    notice += "</div>";
  }

  String html;
  html.reserve(4096);
  html += "<!doctype html><html lang='pt-br'><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Wi-Fi Setup</title>";
  html += "<style>";
  html += "body{font-family:Segoe UI,Arial,sans-serif;background:#0f172a;color:#e2e8f0;margin:0;padding:20px;}";
  html += ".card{max-width:520px;margin:0 auto;background:#111827;border:1px solid #334155;border-radius:14px;padding:18px;}";
  html += "h1{margin:0 0 10px;font-size:1.25rem;} p,li{color:#cbd5e1;} label{display:block;margin:10px 0 6px;}";
  html += "input{width:100%;padding:10px;border:1px solid #475569;border-radius:8px;background:#0b1220;color:#f8fafc;}";
  html += "button{margin-top:14px;width:100%;padding:11px;border:0;border-radius:8px;background:#2563eb;color:#fff;font-weight:700;}";
  html += ".meta{background:#0b1220;border:1px solid #334155;padding:10px;border-radius:8px;margin:12px 0;}";
  html += ".notice{padding:10px;border-radius:8px;margin:10px 0;font-weight:600;}";
  html += ".notice.ok{background:#14532d;border:1px solid #22c55e;} .notice.err{background:#7f1d1d;border:1px solid #ef4444;}";
  html += "</style></head><body><div class='card'>";
  html += "<h1>Configuração de Wi-Fi</h1>";
  html += "<p>Página protegida com o mesmo login/senha do OTA.</p>";
  html += notice;
  html += "<div class='meta'><b>SSID configurado:</b> ";
  html += configuredSsid;
  html += "<br><b>Wi-Fi conectado:</b> ";
  html += wifi_is_connected() ? "SIM" : "NAO";
  html += "<br><b>IP STA:</b> ";
  html += _escape_html(stationIp);
  html += "<br><b>AP recuperação ativo:</b> ";
  html += wifi_recovery_ap_active() ? "SIM" : "NAO";
  html += "<br><b>SSID AP:</b> ";
  html += apSsid.length() ? apSsid : "--";
  html += "<br><b>IP AP:</b> ";
  html += _escape_html(apIp);
  html += "</div>";
  html += "<form method='POST' action='/wifi-setup/save'>";
  html += "<label for='ssid'>Novo SSID</label><input id='ssid' name='ssid' maxlength='32' required>";
  html += "<label for='password'>Nova senha (0 ou 8..63 caracteres)</label>";
  html += "<input id='password' name='password' type='password' maxlength='63'>";
  html += "<button type='submit'>Salvar e reconectar</button></form>";
  html += "<p>Após salvar, o ESP32 tenta reconectar sem pausar controle local. ";
  html += "Quando conectar com sucesso, o AP de recuperação é desativado automaticamente.</p>";
  html += "</div></body></html>";
  return html;
}

/**
 * Estado corrente, para diagnóstico direto no dispositivo.
 *
 * Útil exatamente quando o servidor não está acessível — é o "está vivo e o
 * que ele acha que está acontecendo" sem depender de nada externo.
 */
static String _build_json() {
  JsonDocument doc;

  JsonObject light_obj  = doc["light"].to<JsonObject>();
  light_obj["on"]       = light_get_state();
  light_obj["source"]   = light_source_name(light_get_source());

  JsonObject temp_obj   = doc["temperature"].to<JsonObject>();
  temp_obj["available"] = temperature_available();
  temp_obj["valid"]     = temperature_is_fresh();
  temp_obj["age_ms"]    = temperature_age_ms();
  if (temperature_is_fresh()) {
    temp_obj["celsius"] = serialized(String(temperature_read(), 1));
  } else {
    temp_obj["celsius"] = nullptr;
  }

  JsonObject fan_obj       = doc["fan"].to<JsonObject>();
  fan_obj["on"]            = fan_is_on();
  fan_obj["speed_percent"] = fan_get_speed_percent();
  fan_obj["rpm"]           = fan_get_rpm();
  fan_obj["mode"]          = fan_mode_name(fan_get_mode_report());
  // Só no firmware de debug: o ADC do pot (já filtrado) e o botão (já com
  // debounce), os mesmos valores do `diag` da telemetria, aqui sem depender
  // do servidor estar de pé.
  fan_obj["pot_raw_adc"]   = fan_get_pot_raw_adc();
  doc["button_pressed"]    = light_button_pressed();

  JsonObject rtc_obj   = doc["rtc"].to<JsonObject>();
  rtc_obj["time"]      = rtc_get_time_str();
  rtc_obj["available"] = rtc_available();
  rtc_obj["lost_power"] = rtc_lost_power();

  JsonObject cfg_obj    = doc["config"].to<JsonObject>();
  cfg_obj["version"]    = device_config_version();
  cfg_obj["on_time"]    = device_config_on_time();
  cfg_obj["off_time"]   = device_config_off_time();
  DeviceConfig cfg_atual = device_config_snapshot();
  cfg_obj["trigger_c"]  = serialized(String(cfg_atual.fan_trigger_c, 1));
  cfg_obj["off_c"]      = serialized(String(cfg_atual.fan_off_c, 1));

  JsonObject srv_obj        = doc["server"].to<JsonObject>();
  srv_obj["failures"]       = api_client_consecutive_failures();
  srv_obj["last_success_ms"] = api_client_last_success_ms();
  srv_obj["last_http"]      = api_client_last_http_status();
  srv_obj["pending_events"] = event_log_pending();

  doc["fw_version"] = FW_VERSION;
  doc["device_id"]  = DEVICE_ID;
  doc["uptime_ms"]  = millis();

  String out;
  serializeJson(doc, out);
  return out;
}

void webserver_init() {
  _ota_load_last_result();

  // Headers globais (CORS por origem permitida é aplicado por requisição)
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, X-Api-Token");

  if (_auth_enabled()) {
    Serial.println("[WebServer] Autenticacao da API habilitada (X-Api-Token/token)");
  } else {
    Serial.println("[WebServer] AVISO: API sem token de autenticacao (API_AUTH_TOKEN vazio)");
  }

  // GET /status — consulta estado sem alterar nada
  _server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!_require_auth(request)) return;
    _send_json(request, 200, _build_json());
  });

  // GET /debug — anel de eventos, memória e pilha, e as chaves de experimento
  // (ver firmware/bettacare-debug/README.md):
  //   ?temp=0|1  ?temp_ms=N  ?rtc=0|1  ?feeder=0|1           liga/desliga subsistemas
  //   ?rtc_fail=N  ?rtc_bad=N  ?rtc_offset=MIN&rtc_offset_reads=N  ?temp85=N
  //                                                           injeção de falhas
  //   ?diag=1                                                 autodiagnóstico agora
  //   ?hang=1                                                 trava o loop (teste do watchdog)
  _server.on("/debug", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!_require_auth(request)) return;
    if (request->hasParam("temp")) {
      dbg_temp_enabled = request->getParam("temp")->value().toInt() != 0;
    }
    if (request->hasParam("temp_ms")) {
      long v = request->getParam("temp_ms")->value().toInt();
      if (v >= 1000 && v <= 60000) dbg_temp_interval_ms = (uint32_t)v;
    }
    if (request->hasParam("rtc")) {
      dbg_rtc_enabled = request->getParam("rtc")->value().toInt() != 0;
    }
    if (request->hasParam("feeder")) {
      dbg_feeder_enabled = request->getParam("feeder")->value().toInt() != 0;
    }
    if (request->hasParam("rtc_fail")) {
      dbg_rtc_fail_reads = (uint32_t)request->getParam("rtc_fail")->value().toInt();
    }
    if (request->hasParam("rtc_bad")) {
      dbg_rtc_bad_reads = (uint32_t)request->getParam("rtc_bad")->value().toInt();
    }
    if (request->hasParam("rtc_offset")) {
      dbg_rtc_offset_min = (int32_t)request->getParam("rtc_offset")->value().toInt();
      dbg_rtc_offset_reads = request->hasParam("rtc_offset_reads")
          ? (uint32_t)request->getParam("rtc_offset_reads")->value().toInt()
          : 1;
    }
    if (request->hasParam("temp85")) {
      dbg_temp_fake_85 = (uint32_t)request->getParam("temp85")->value().toInt();
    }
    if (request->hasParam("diag")) {
      dbg_diag_request = request->getParam("diag")->value().toInt() != 0;
    }
    if (request->hasParam("hang")) {
      dbg_hang_loop = request->getParam("hang")->value().toInt() == 1;
    }
    _send_json(request, 200, dbg_dump_json());
  });

  // Página de recuperação de Wi-Fi (HTTP Basic com credenciais OTA)
  _server.on("/wifi-setup", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!_require_setup_auth(request)) return;
    request->send(200, "text/html; charset=utf-8", _build_wifi_setup_page("", false));
  });

  // Recebe novas credenciais Wi-Fi e dispara reconexão sem bloquear o sistema.
  _server.on("/wifi-setup/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!_require_setup_auth(request)) return;

    const AsyncWebParameter *ssidParam = request->hasParam("ssid", true)
      ? request->getParam("ssid", true)
      : (request->hasParam("ssid") ? request->getParam("ssid") : nullptr);
    const AsyncWebParameter *passParam = request->hasParam("password", true)
      ? request->getParam("password", true)
      : (request->hasParam("password") ? request->getParam("password") : nullptr);

    if (ssidParam == nullptr || passParam == nullptr) {
      request->send(400, "text/html; charset=utf-8",
                    _build_wifi_setup_page("Parametros obrigatorios ausentes.", true));
      return;
    }

    String newSsid = ssidParam->value();
    String newPass = passParam->value();
    bool ok = wifi_set_credentials(newSsid, newPass);

    if (!ok) {
      request->send(400, "text/html; charset=utf-8",
                    _build_wifi_setup_page("Credenciais invalidas. SSID 1..32 e senha 0 ou 8..63.", true));
      return;
    }

    request->send(200, "text/html; charset=utf-8",
                  _build_wifi_setup_page("Credenciais salvas. Tentando conectar...", false));
  });

  // Handler para preflight CORS (OPTIONS) e 404
  _server.onNotFound([](AsyncWebServerRequest *request) {
    if (request->method() == HTTP_OPTIONS) {
      AsyncWebServerResponse *response = request->beginResponse(204);
      _add_cors_headers(request, response);
      request->send(response);
    } else if (wifi_recovery_ap_active() && request->method() == HTTP_GET) {
      request->redirect("/wifi-setup");
    } else {
      _send_json(request, 404, "{\"error\":\"Not found\"}");
    }
  });

  // OTA — interface de atualização em http://<IP>/update
  ElegantOTA.begin(&_server, OTA_USERNAME, OTA_PASSWORD);

  ElegantOTA.onStart([]() {
    Serial.println("[OTA] Atualizacao de firmware iniciada");
    event_log(SEV_INFO, COMP_OTA, "ota.started",
              "Atualizacao de firmware iniciada via OTA");
  });

  ElegantOTA.onProgress([](size_t current, size_t total) {
    if (millis() - _ota_progress_ms > 1000) {
      _ota_progress_ms = millis();
      Serial.printf("[OTA] Progresso: %u / %u bytes (%.0f%%)\n",
                    current, total, (float)current / total * 100.0f);
    }
  });

  ElegantOTA.onEnd([](bool success) {
    // Grava **antes** de logar: o sucesso reinicia o dispositivo em seguida, e
    // o evento provavelmente não chega a sair num POST. O que sobrevive ao
    // reboot é o valor na flash, e é dele que o `diag` do próximo POST conta a
    // história.
    _ota_store_result(success ? "ok" : "failed");

    if (success) {
      Serial.println("[OTA] Concluido com sucesso! Reiniciando...");
      event_log(SEV_INFO, COMP_OTA, "ota.succeeded",
                "Atualizacao concluida; reiniciando com o firmware novo");
    } else {
      Serial.println("[OTA] Falha na atualizacao.");
      event_log(SEV_ERROR, COMP_OTA, "ota.failed",
                "Atualizacao falhou; seguindo com o firmware anterior");
    }
  });

  _server.begin();
  Serial.println("[WebServer] Servidor iniciado na porta 80");
  if (wifi_is_connected()) {
    Serial.printf("[OTA] Interface disponivel em http://%s/update\n",
                  WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[OTA] WiFi ainda nao conectado. OTA/API ficam disponiveis quando houver IP STA.");
  }
  Serial.println("[WiFi] Portal de configuracao: /wifi-setup (login/senha OTA)");
}

void webserver_loop() {
  ElegantOTA.loop();
}
