#include "wifi_manager.h"
#include "config.h"
#include <WiFi.h>

static unsigned long _last_attempt_ms = 0;
static uint16_t      _reconnect_count = 0;
static bool          _was_connected   = false;

void wifi_connect() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_ID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  _last_attempt_ms = millis();
  Serial.printf("[WiFi] Conectando a \"%s\"...\n", WIFI_SSID);
}

void wifi_check_reconnect() {
  bool connected = WiFi.status() == WL_CONNECTED;

  if (connected && !_was_connected) {
    Serial.printf("[WiFi] Conectado. IP: %s   MAC: %s\n",
                  WiFi.localIP().toString().c_str(), WiFi.macAddress().c_str());
  } else if (!connected && _was_connected) {
    _reconnect_count++;
    Serial.println("[WiFi] Conexao perdida.");
  }
  _was_connected = connected;

  if (connected) return;
  if (millis() - _last_attempt_ms < WIFI_RECONNECT_INTERVAL_MS) return;

  _last_attempt_ms = millis();
  Serial.println("[WiFi] Tentando reconectar...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

bool wifi_is_connected() { return WiFi.status() == WL_CONNECTED; }
uint16_t wifi_reconnect_count() { return _reconnect_count; }
