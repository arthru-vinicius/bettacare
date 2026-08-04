#include "web_server.h"
#include "config.h"
#include "light.h"
#include "temperature.h"
#include "fan.h"
#include "rtc_manager.h"
#include "wifi_manager.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <ElegantOTA.h>
#include <WiFi.h>
#include <ctype.h>
#include <string.h>

static AsyncWebServer _server(80);

// Rastreia o início da última atualização OTA para log de progresso
static unsigned long _ota_progress_ms = 0;

#ifndef API_AUTH_TOKEN
#define API_AUTH_TOKEN ""
#endif

#ifndef CORS_ALLOWED_ORIGIN
#define CORS_ALLOWED_ORIGIN ""
#endif

static bool _is_valid_percent_param(const String &value) {
  if (value.isEmpty() || value.length() > 3) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    if (!isdigit((unsigned char)value[i])) return false;
  }
  int pct = value.toInt();
  return pct >= 0 && pct <= 100;
}

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

static bool _is_authorized(AsyncWebServerRequest *request) {
  if (!_auth_enabled()) return true;

  String provided;
  if (request->hasHeader("X-Api-Token")) {
    provided = request->header("X-Api-Token");
  } else if (request->hasParam("token")) {
    provided = request->getParam("token")->value();
  }

  return provided.equals(API_AUTH_TOKEN);
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
 * @brief Constrói o JSON de estado completo do sistema e serializa para String.
 * Formato:
 * {
 *   "light":       { "on": bool },
 *   "temperature": { "celsius": float|null, "available": bool, "valid": bool },
 *   "fan":         { "on": bool, "speed_percent": int, "rpm": int,
 *                    "trigger_c": float, "off_c": float },
 *   "rtc":         { "time": "HH:MM", "available": bool,
 *                    "on_time": "HH:MM", "off_time": "HH:MM" }
 * }
 */
static String _build_json() {
  DynamicJsonDocument doc(512);

  // Luminária
  JsonObject light_obj = doc.createNestedObject("light");
  light_obj["on"]      = light_get_state();

  // Temperatura
  JsonObject temp_obj   = doc.createNestedObject("temperature");
  bool temp_avail       = temperature_available();
  bool temp_valid       = temperature_is_fresh();
  temp_obj["available"] = temp_avail;
  temp_obj["valid"]     = temp_valid;
  if (temp_valid) {
    temp_obj["celsius"] = serialized(String(temperature_read(), 1));
  } else {
    temp_obj["celsius"] = (char*)nullptr;  // null no JSON
  }

  // Ventoinha
  JsonObject fan_obj       = doc.createNestedObject("fan");
  fan_obj["on"]            = fan_is_on();
  fan_obj["speed_percent"] = fan_get_speed_percent();
  fan_obj["rpm"]           = fan_get_rpm();
  fan_obj["trigger_c"]     = serialized(String(fan_get_trigger_c(), 1));
  fan_obj["off_c"]         = serialized(String(fan_get_off_c(), 1));

  // RTC
  JsonObject rtc_obj   = doc.createNestedObject("rtc");
  rtc_obj["time"]      = rtc_get_time_str();
  rtc_obj["available"] = rtc_available();
  rtc_obj["on_time"]   = rtc_get_on_time();
  rtc_obj["off_time"]  = rtc_get_off_time();

  String response;
  serializeJson(doc, response);
  return response;
}

void webserver_init() {
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

  // GET /toggle — alterna luminária sem cancelar a automação por horário.
  // O schedule continua ativo: na próxima transição de período a luminária
  // voltará a seguir o horário configurado em config.h.
  _server.on("/toggle", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!_require_auth(request)) return;
    light_toggle();
    Serial.println("[WebServer] Luminaria alternada via web");
    _send_json(request, 200, _build_json());
  });

  // GET /temperature — retorna apenas leitura de temperatura
  _server.on("/temperature", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!_require_auth(request)) return;
    DynamicJsonDocument doc(128);
    bool avail       = temperature_available();
    bool valid       = temperature_is_fresh();
    doc["available"] = avail;
    doc["valid"]     = valid;
    if (valid) {
      doc["celsius"] = serialized(String(temperature_read(), 1));
    } else {
      doc["celsius"] = (char*)nullptr;
    }
    String response;
    serializeJson(doc, response);
    _send_json(request, 200, response);
  });

  // GET /fan_toggle — alterna ventoinha; congela controle automático até próximo RTC_ON.
  // Liga na última velocidade manual se estiver desligada; desliga se estiver ligada.
  _server.on("/fan_toggle", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!_require_auth(request)) return;
    fan_toggle_web();
    Serial.println("[WebServer] Ventoinha alternada via web");
    _send_json(request, 200, _build_json());
  });

  // GET /fan_speed?value=0..100 — define velocidade da ventoinha via web.
  // Congela controle automático até próximo RTC_ON. value=0 equivale a desligar.
  _server.on("/fan_speed", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!_require_auth(request)) return;

    if (!request->hasParam("value")) {
      _send_json(request, 400, "{\"error\":\"Parametro value obrigatorio (0-100)\"}");
      return;
    }

    String rawValue = request->getParam("value")->value();
    if (!_is_valid_percent_param(rawValue)) {
      _send_json(request, 400, "{\"error\":\"Parametro value invalido (use inteiro 0-100)\"}");
      return;
    }

    int pct = rawValue.toInt();
    fan_set_speed_web(pct);
    Serial.printf("[WebServer] Velocidade da ventoinha: %d%%\n", pct);
    _send_json(request, 200, _build_json());
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
  });

  ElegantOTA.onProgress([](size_t current, size_t total) {
    if (millis() - _ota_progress_ms > 1000) {
      _ota_progress_ms = millis();
      Serial.printf("[OTA] Progresso: %u / %u bytes (%.0f%%)\n",
                    current, total, (float)current / total * 100.0f);
    }
  });

  ElegantOTA.onEnd([](bool success) {
    if (success) {
      Serial.println("[OTA] Concluido com sucesso! Reiniciando...");
    } else {
      Serial.println("[OTA] Falha na atualizacao.");
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
