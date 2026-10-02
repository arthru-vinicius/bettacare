#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#define ELEGANTOTA_USE_ASYNC_WEBSERVER 1
#include <ElegantOTA.h>
#include "driver/rtc_io.h"

// Sketch de diagnóstico temporário — isola GPIO23 (relé) e GPIO27 (corte da
// ventoinha) de TUDO que o firmware principal faz: sem I2C, sem ADC do pot,
// sem PWM, sem task de rede, sem servidor remoto. Só Wi-Fi + os dois pinos
// alternando em padrão previsível, com leitura de volta a cada volta do loop.
//
// Se "mismatches" ficar em 0 por vários minutos, o hardware/fiação desses
// dois pinos está saudável — o bug está na complexidade do firmware
// principal. Se crescer, é perturbação elétrica real nesses pinos,
// independente de qualquer lógica de software.
//
// OTA em /update (mesmo fluxo já validado no firmware principal:
// GET /ota/start depois POST multipart em /ota/upload, com a mesma
// autenticação HTTP Basic).
//
// Credenciais em `config.h` (copie de `config.example.h`), fora do git: o
// repositório é público.

#include "config.h"

#define PIN_SSR       23
#define PIN_FAN_POWER 27
#define PIN_FAN_POWER_CANDIDATO 26  // candidato novo, pra comparar lado a lado com o 27

AsyncWebServer server(80);

bool ssrWant = false;
bool fanWant = true;
unsigned long lastToggleSsr = 0;
unsigned long lastToggleFan = 0;
const unsigned long SSR_INTERVAL_MS = 4000;
const unsigned long FAN_INTERVAL_MS = 7000;

uint32_t ssrMismatches = 0;
uint32_t fanMismatches = 0;
uint32_t candMismatches = 0;
uint32_t loopCount      = 0;

void checkPin(int pin, bool want, uint32_t &mismatches) {
  if (digitalRead(pin) != want) mismatches++;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_SSR, OUTPUT);
  digitalWrite(PIN_SSR, LOW);

  // GPIO27 é pino de domínio RTC — o subsistema RTC pode continuar
  // segurando o pad mesmo depois do pinMode()/digitalWrite() comuns.
  // Libera explicitamente antes de configurar como saída digital normal.
  rtc_gpio_deinit((gpio_num_t)PIN_FAN_POWER);
  pinMode(PIN_FAN_POWER, OUTPUT);
  digitalWrite(PIN_FAN_POWER, HIGH);

  // Candidato novo — mesmo padrão, pino diferente, pra comparar lado a lado.
  pinMode(PIN_FAN_POWER_CANDIDATO, OUTPUT);
  digitalWrite(PIN_FAN_POWER_CANDIDATO, HIGH);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("[Debug] Conectando Wi-Fi...");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(200);
  Serial.printf("[Debug] IP: %s\n", WiFi.localIP().toString().c_str());

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{";
    json += "\"ssr_want\":" + String(ssrWant ? "true" : "false") + ",";
    json += "\"ssr_real\":" + String((bool)digitalRead(PIN_SSR) ? "true" : "false") + ",";
    json += "\"ssr_mismatches\":" + String(ssrMismatches) + ",";
    json += "\"fan_want\":" + String(fanWant ? "true" : "false") + ",";
    json += "\"fan_real\":" + String((bool)digitalRead(PIN_FAN_POWER) ? "true" : "false") + ",";
    json += "\"fan_mismatches\":" + String(fanMismatches) + ",";
    json += "\"cand_real\":" + String((bool)digitalRead(PIN_FAN_POWER_CANDIDATO) ? "true" : "false") + ",";
    json += "\"cand_mismatches\":" + String(candMismatches) + ",";
    json += "\"loop_count\":" + String(loopCount) + ",";
    json += "\"uptime_ms\":" + String(millis());
    json += "}";
    request->send(200, "application/json", json);
  });

  ElegantOTA.begin(&server, OTA_USERNAME, OTA_PASSWORD);
  server.begin();

  lastToggleSsr = millis();
  lastToggleFan = millis();
}

void loop() {
  ElegantOTA.loop();
  loopCount++;

  checkPin(PIN_SSR, ssrWant, ssrMismatches);
  checkPin(PIN_FAN_POWER, fanWant, fanMismatches);
  checkPin(PIN_FAN_POWER_CANDIDATO, fanWant, candMismatches);

  unsigned long now = millis();
  if (now - lastToggleSsr >= SSR_INTERVAL_MS) {
    lastToggleSsr = now;
    ssrWant = !ssrWant;
    digitalWrite(PIN_SSR, ssrWant ? HIGH : LOW);
  }
  if (now - lastToggleFan >= FAN_INTERVAL_MS) {
    lastToggleFan = now;
    fanWant = !fanWant;
    digitalWrite(PIN_FAN_POWER, fanWant ? HIGH : LOW);
    digitalWrite(PIN_FAN_POWER_CANDIDATO, fanWant ? HIGH : LOW);
  }
}
