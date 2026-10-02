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

// --- Pinagem (docs/pinagem-alimentador-modulo.md) ------------------------------
// Os dez GPIOs sem função de boot da Super Mini, todos em uso. Fora daqui:
// GPIO2, GPIO8 e GPIO9 são strapping (o 8 é o LED da placa, o 9 o BOOT), o
// GPIO11 alimenta a flash e o GPIO18/19 é o USB.
#define PIN_M1           0    // vibração anti-empacamento (BC337); PWM — motor de 3 V
#define PIN_M2           1    // vibração de aviso (BC337); PWM com partida suave
#define PIN_IR_SENSOR    3    // saída do LM393: entrada, interrupção
#define PIN_IR_LED       4    // LED IR, aceso só durante a dosagem e o autoteste
#define PIN_BUTTON       5    // INPUT_PULLUP, amostrado por timer (2 ms / 30 ms)
#define PIN_I2C_SDA      6    // DS3231 (0x68), EEPROM do ZS-042 (0x57), SSD1306 (0x3C)
#define PIN_I2C_SCL      7
#define PIN_STATUS_LED   8    // LED da placa, aceso em nível baixo
#define PIN_SERVO       10    // MG90S; PWM só durante o movimento
#define PIN_LINK_RX     20    // UART0 ← TX do principal (GPIO4 de lá)
#define PIN_LINK_TX     21    // UART0 → RX do principal (GPIO16 de lá)
#define LINK_BAUD     9600
