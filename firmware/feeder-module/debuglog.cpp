#include "debuglog.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

DebugLog Log;

static const uint32_t MAGIA = 0xBE77A10Cu;
static const uint16_t RING = 3072;

/**
 * Fora da inicialização da RAM (RTC_NOINIT): o conteúdo atravessa reinício
 * por watchdog, exceção, queda de tensão ou OTA. Faltou energia, a mágica não
 * confere e o anel recomeça.
 */
struct Anel {
  uint32_t magia;
  uint32_t seq;      // número da próxima linha
  uint16_t cabeca;   // próxima posição de escrita
  uint8_t  cheio;
  char     dados[RING];
};
static RTC_NOINIT_ATTR Anel _anel;

static SemaphoreHandle_t _mtx = nullptr;
static char   _linha[192];
static size_t _len = 0;

#ifdef DEBUG_LOG_HOST
#ifndef DEBUG_LOG_PORT
#define DEBUG_LOG_PORT 5514
#endif
static WiFiUDP   _udp;
static IPAddress _destino;
static bool      _destino_ok = false;
static const char ID[] = DEVICE_ID " ";
#endif

static void _poe(const char* s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    _anel.dados[_anel.cabeca] = s[i];
    _anel.cabeca = (uint16_t)((_anel.cabeca + 1) % RING);
    if (_anel.cabeca == 0) _anel.cheio = 1;
  }
}

/** Uma linha completa: número, segundos desde o boot e o texto. */
static void _fecha_linha() {
  char pre[24];
  int p = snprintf(pre, sizeof(pre), "%lu %lu ", (unsigned long)_anel.seq++, (unsigned long)(millis() / 1000));
  if (p < 0) p = 0;
  _poe(pre, (size_t)p);
  _poe(_linha, _len);
  _poe("\n", 1);
#ifdef DEBUG_LOG_HOST
  // Socket não bloqueante: sem o PC ouvindo, o datagrama só se perde.
  if (_destino_ok && WiFi.status() == WL_CONNECTED && _udp.beginPacket(_destino, DEBUG_LOG_PORT)) {
    _udp.write((const uint8_t*)ID, sizeof(ID) - 1);
    _udp.write((const uint8_t*)pre, (size_t)p);
    _udp.write((const uint8_t*)_linha, _len);
    _udp.endPacket();
  }
#endif
  _len = 0;
}

size_t DebugLog::write(const uint8_t* buf, size_t len) {
  Serial.write(buf, len);
  if (!_mtx) return len;   // antes do log_init(): só a USB
  // O loop nunca espera o log: com a trava ocupada por mais que isto, a linha
  // fica só na USB.
  if (xSemaphoreTake(_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return len;
  for (size_t i = 0; i < len; i++) {
    char c = (char)buf[i];
    if (c == '\r') continue;
    if (c == '\n') {
      _fecha_linha();
      continue;
    }
    _linha[_len++] = c;
    if (_len >= sizeof(_linha) - 1) _fecha_linha();
  }
  xSemaphoreGive(_mtx);
  return len;
}

size_t DebugLog::write(uint8_t c) { return write(&c, 1); }

void log_init() {
  bool sobreviveu = _anel.magia == MAGIA && _anel.cabeca < RING && _anel.cheio <= 1 && _anel.seq > 0;
  if (!sobreviveu) {
    memset(&_anel, 0, sizeof(_anel));
    _anel.magia = MAGIA;
    _anel.seq = 1;
  }
  _mtx = xSemaphoreCreateMutex();
#ifdef DEBUG_LOG_HOST
  _destino_ok = _destino.fromString(DEBUG_LOG_HOST);
#endif
  if (sobreviveu) Log.printf("----- reinicio (%s); as linhas acima sao de antes -----\n", log_reset_reason());
}

uint32_t log_next_seq() { return _anel.seq; }

String log_recent(uint32_t since) {
  String out;
  if (!_mtx || xSemaphoreTake(_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return out;
  size_t n = _anel.cheio ? RING : _anel.cabeca;
  size_t inicio = _anel.cheio ? _anel.cabeca : 0;
  out.reserve(n + 1);
  bool parcial = _anel.cheio;   // o anel deu a volta: a primeira linha está cortada
  char linha[sizeof(_linha) + 24];
  size_t len = 0;
  for (size_t i = 0; i < n; i++) {
    char c = _anel.dados[(inicio + i) % RING];
    if (parcial) {
      if (c == '\n') parcial = false;
      continue;
    }
    if (c != '\n') {
      if (len < sizeof(linha) - 1) linha[len++] = c;
      continue;
    }
    linha[len] = '\0';
    if (strtoul(linha, nullptr, 10) > since) {
      out += linha;
      out += '\n';
    }
    len = 0;
  }
  xSemaphoreGive(_mtx);
  return out;
}

const char* log_reset_reason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "ligado na energia";
    case ESP_RST_SW:        return "pedido pelo firmware: OTA ou comando";
    case ESP_RST_PANIC:     return "EXCECAO (panic)";
    case ESP_RST_INT_WDT:   return "WATCHDOG de interrupcao";
    case ESP_RST_TASK_WDT:  return "WATCHDOG do loop";
    case ESP_RST_WDT:       return "WATCHDOG";
    case ESP_RST_BROWNOUT:  return "QUEDA DE TENSAO (brownout)";
    case ESP_RST_EXT:       return "pino de reset";
    case ESP_RST_DEEPSLEEP: return "volta do deep sleep";
    default:                return "outro";
  }
}
