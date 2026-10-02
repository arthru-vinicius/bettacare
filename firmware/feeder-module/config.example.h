#pragma once

// =============================================================================
// CONFIGURAÇÃO — copie para config.h e preencha os valores reais.
// config.h está no .gitignore e nunca deve ser versionado: este repositório
// é público.
// =============================================================================
//
// Firmware do módulo alimentador (ESP32-C3 Super Mini). Comportamento (agenda
// de fábrica, limites, tempos dos motores) fica em params.h, versionado; aqui
// só o que é deste módulo em particular: identidade, rede, senha e pinos.
// Bancada e gravação: README.md desta pasta. Montagem:
// docs/pinagem-alimentador-modulo.md.
//
// Placa (arduino-cli): esp32:esp32:esp32c3:CDCOnBoot=cdc
// Bibliotecas (Arduino Library Manager):
//   U8g2, ESPAsyncWebServer, AsyncTCP, ElegantOTA

// --- Identidade do dispositivo -----------------------------------------------
#define DEVICE_ID      "feeder-01"
#define FW_VERSION     "1.0.0-beta"

// --- Wi-Fi ---------------------------------------------------------------
// Mesma rede do ESP32 principal — o C3 só enxerga 2,4 GHz. Sem Wi-Fi o
// módulo alimenta do mesmo jeito: Wi-Fi é só para OTA e para acertar o
// relógio pelo NTP.
#define WIFI_SSID      "YOUR_NETWORK_HERE"
#define WIFI_PASSWORD  "YOUR_PASSWORD_HERE"
#define WIFI_RECONNECT_INTERVAL_MS  10000UL   // tentativa de reconexão (não-bloqueante)

// --- OTA -------------------------------------------------------------------
// Interface de atualização em http://<IP_DO_MODULO>/update
// Credenciais PRÓPRIAS deste módulo — não reaproveite as do ESP32 principal.
#define OTA_USERNAME   "admin"
#define OTA_PASSWORD   "REPLACE_WITH_STRONG_PASSWORD"

// --- Log pela rede (opcional) ---------------------------------------------------
// Cada linha do log também sai num datagrama UDP para o PC da bancada — para
// acompanhar o módulo sem o cabo USB (receptor em tools/udplog.mjs). Sem isto,
// o log segue na USB e em GET /log.
// #define DEBUG_LOG_HOST  "192.168.1.50"   // o IP do PC
// #define DEBUG_LOG_PORT  5514

// --- Relógio (opcional) --------------------------------------------------------
// O padrão é Brasília sem horário de verão e os servidores do NTP.br; descomente
// para mudar. A hora do módulo é a do aquário — a agenda roda nela.
// #define NTP_SERVER_1        "a.st1.ntp.br"
// #define NTP_SERVER_2        "pool.ntp.org"
// #define CLOCK_UTC_OFFSET_S  (-3L * 3600L)

// --- Pinagem (docs/pinagem-alimentador-modulo.md) ------------------------------
// Fora daqui: GPIO2, GPIO8 e GPIO9 são strapping (o 8 é o LED da placa, o 9 o
// BOOT), o GPIO11 alimenta a flash e o GPIO18/19 é o USB. O GPIO2 e o GPIO9
// ficam livres.
#define PIN_M1           0    // vibração anti-empacamento (BC337); PWM — motor de 3 V
#define PIN_M2           1    // vibração de aviso (BC337); PWM com partida suave
#define PIN_IR_SENSOR    3    // saída do LM393: entrada, interrupção
#define PIN_IR_LED       4    // LED IR, aceso só durante a dosagem e o autoteste
#define PIN_BUTTON       5    // INPUT_PULLUP, amostrado por timer (2 ms / 30 ms)
#define PIN_I2C_SDA      6    // DS3231 (0x68), EEPROM do ZS-042 (0x57), SSD1306 (0x3C)
#define PIN_I2C_SCL      7
#define PIN_STATUS_LED   8    // LED da placa, aceso em nível baixo
#define PIN_SERVO       10    // MG90S; PWM só durante o movimento
#define PIN_LINK_RX     20    // UART1 ← TX do principal (GPIO4 de lá); pinos da UART0, que é o console
#define PIN_LINK_TX     21    // UART1 → RX do principal (GPIO16 de lá)
#define LINK_BAUD     9600
