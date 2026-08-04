#include <Arduino.h>
#include <ArduinoJson.h>
#include <stdarg.h>
#include "log_manager.h"
#include "rtc_manager.h"  // rtc_get_time_str()

// Capacidade do buffer circular (entradas mais antigas são descartadas)
static const uint8_t LOG_MAX = 30;

struct LogEntry {
  char time[6];   // "HH:MM\0"
  char msg[100];  // mensagem truncada se necessário
};

static LogEntry _buf[LOG_MAX];
static uint8_t  _head     = 0;   // próximo slot a escrever (circular)
static uint8_t  _count    = 0;   // entradas válidas no buffer (≤ LOG_MAX)
static bool     _pending  = false;
static char     _last_msg[100]  = "";  // deduplicação: ignora repetição imediata

void log_init() {
  memset(_buf,      0, sizeof(_buf));
  memset(_last_msg, 0, sizeof(_last_msg));
  _head    = 0;
  _count   = 0;
  _pending = false;
}

void log_event(const char* msg) {
  // Deduplicação: ignora se igual à última mensagem inserida
  if (strncmp(msg, _last_msg, sizeof(_last_msg) - 1) == 0) return;

  // Hora via RTC ("HH:MM") ou "--:--" se módulo indisponível
  String t = rtc_get_time_str();
  strncpy(_buf[_head].time, t.c_str(), 5);
  _buf[_head].time[5] = '\0';

  strncpy(_buf[_head].msg, msg, sizeof(_buf[_head].msg) - 1);
  _buf[_head].msg[sizeof(_buf[_head].msg) - 1] = '\0';

  // Atualiza deduplicação
  strncpy(_last_msg, msg, sizeof(_last_msg) - 1);
  _last_msg[sizeof(_last_msg) - 1] = '\0';

  // Avança o ponteiro circular
  _head = (_head + 1) % LOG_MAX;
  if (_count < LOG_MAX) _count++;
  _pending = true;

  // Espelha no Serial para monitoramento local
  Serial.print("[LOG] ");
  Serial.println(msg);
}

void log_eventf(const char* fmt, ...) {
  char tmp[100];
  va_list args;
  va_start(args, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, args);
  va_end(args);
  log_event(tmp);
}

String log_get_json() {
  // Dimensionado para LOG_MAX × ~120 bytes de entrada + overhead
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();

  // Reordena do mais antigo ao mais recente
  uint8_t start = (_count < LOG_MAX) ? 0 : _head;
  for (uint8_t i = 0; i < _count; i++) {
    uint8_t idx = (start + i) % LOG_MAX;
    JsonObject entry = arr.createNestedObject();
    entry["t"] = _buf[idx].time;
    entry["m"] = _buf[idx].msg;
  }

  String out;
  serializeJson(doc, out);
  return out;
}

void log_clear() {
  memset(_buf,      0, sizeof(_buf));
  memset(_last_msg, 0, sizeof(_last_msg));
  _head    = 0;
  _count   = 0;
  _pending = true;  // força publicação do array vazio no MQTT
}

bool log_has_pending()    { return _pending; }
void log_mark_published() { _pending = false; }
