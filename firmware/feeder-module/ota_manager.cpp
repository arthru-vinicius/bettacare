#include "ota_manager.h"
#include "config.h"
#include "wifi_manager.h"
#include <ESPAsyncWebServer.h>
#define ELEGANTOTA_USE_ASYNC_WEBSERVER 1
#include <ElegantOTA.h>
#include <WiFi.h>

static AsyncWebServer _server(80);
static unsigned long  _ota_progress_ms = 0;

static String _status_text() {
  String out;
  out.reserve(192);
  out += "device_id: " DEVICE_ID "\n";
  out += "fw_version: " FW_VERSION "\n";
  out += "uptime_ms: " + String(millis()) + "\n";
  out += "wifi_connected: " + String(wifi_is_connected() ? "true" : "false") + "\n";
  out += "ip: " + (wifi_is_connected() ? WiFi.localIP().toString() : String("--")) + "\n";
  out += "mac: " + WiFi.macAddress() + "\n";
  out += "rssi_dbm: " + String(wifi_is_connected() ? WiFi.RSSI() : 0) + "\n";
  out += "reconnects: " + String(wifi_reconnect_count()) + "\n";
  return out;
}

void ota_manager_init() {
  _server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain; charset=utf-8", _status_text());
  });

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
    Serial.println(success ? "[OTA] Concluido com sucesso! Reiniciando..."
                            : "[OTA] Falha na atualizacao.");
  });

  _server.begin();
  Serial.println("[WebServer] Servidor iniciado na porta 80");
}

void ota_manager_loop() {
  ElegantOTA.loop();
}
