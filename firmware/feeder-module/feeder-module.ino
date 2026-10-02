#include "config.h"
#include "ota_manager.h"
#include "wifi_manager.h"
#include <WiFi.h>

// Firmware do módulo alimentador (ESP32-C3) — fase de bootstrap: só
// Wi-Fi + OTA. O protocolo UART com o ESP32 principal e o ciclo físico do
// doseador (servo, sensores, botão, OLED) ainda faltam — ver
// docs/pinagem-alimentador-modulo.md e docs/pinagem-e-montagem-esp32.md §8.

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.printf("[Boot] %s fw=%s\n", DEVICE_ID, FW_VERSION);

  wifi_connect();
  Serial.printf("[Boot] MAC: %s\n", WiFi.macAddress().c_str());

  ota_manager_init();
}

void loop() {
  wifi_check_reconnect();
  ota_manager_loop();
}
