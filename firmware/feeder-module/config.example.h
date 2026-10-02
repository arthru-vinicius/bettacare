#pragma once

// =============================================================================
// CONFIGURAÇÃO — copie para config.h e preencha os valores reais.
// config.h está no .gitignore e nunca deve ser versionado: este repositório
// é público.
// =============================================================================
//
// Firmware do módulo alimentador (ESP32-C3), em fase de bootstrap: só
// Wi-Fi + OTA por enquanto. O protocolo UART com o ESP32 principal e o
// ciclo físico do doseador (servo, sensores, botão, OLED) ainda faltam —
// ver docs/pinagem-alimentador-modulo.md e docs/pinagem-e-montagem-esp32.md §8.
//
// Bibliotecas necessárias (Arduino Library Manager):
//   ESPAsyncWebServer, AsyncTCP, ElegantOTA

// --- Identidade do dispositivo -----------------------------------------------
#define DEVICE_ID      "feeder-01"
#define FW_VERSION     "0.1.0-bootstrap"

// --- Wi-Fi ---------------------------------------------------------------
// Mesma rede do ESP32 principal — o C3 só enxerga 2,4 GHz.
#define WIFI_SSID      "YOUR_NETWORK_HERE"
#define WIFI_PASSWORD  "YOUR_PASSWORD_HERE"
#define WIFI_RECONNECT_INTERVAL_MS  10000UL   // tentativa de reconexão (não-bloqueante)

// --- OTA -------------------------------------------------------------------
// Interface de atualização em http://<IP_DO_MODULO>/update
// Credenciais PRÓPRIAS deste módulo — não reaproveite as do ESP32 principal.
#define OTA_USERNAME   "admin"
#define OTA_PASSWORD   "REPLACE_WITH_STRONG_PASSWORD"
