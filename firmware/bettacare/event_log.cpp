#include "event_log.h"

#include <stdarg.h>
#include <string.h>

#include "rtc_manager.h"

static LogEvent          _buf[EVENT_BUFFER_SIZE];
static uint8_t           _count    = 0;   // eventos válidos, do mais antigo ao mais novo
static bool              _overflow = false;
static SemaphoreHandle_t _mutex    = nullptr;

static const char* const SEV_NAMES[] = {"debug", "info", "warn", "error", "fatal"};

static const char* const COMP_NAMES[] = {
    "system", "wifi", "api", "rtc", "temp", "fan",
    "light",  "button", "pot", "nvs", "ota",
};

void event_log_init() {
  _mutex = xSemaphoreCreateMutex();
  _count = 0;
  _overflow = false;
  memset(_buf, 0, sizeof(_buf));
}

const char* event_severity_name(EventSeverity sev) {
  return SEV_NAMES[(uint8_t)sev <= SEV_FATAL ? (uint8_t)sev : SEV_INFO];
}

const char* event_component_name(EventComponent comp) {
  return COMP_NAMES[(uint8_t)comp <= COMP_OTA ? (uint8_t)comp : COMP_SYSTEM];
}

void event_log(EventSeverity sev, EventComponent comp, const char* code,
               const char* fmt, ...) {
  char msg[EVENT_MSG_LEN];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  // Espelha no Serial antes de qualquer coisa: se o mutex estiver travado ou o
  // buffer cheio, ainda assim o monitor serial mostra o que aconteceu.
  Serial.printf("[%s/%s] %s — %s\n", event_severity_name(sev),
                event_component_name(comp), code, msg);

  if (_mutex == nullptr) return;
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

  // Deduplicação: mesma origem e mesmo código na entrada mais recente vira
  // contador, não linha nova.
  if (_count > 0) {
    LogEvent& last = _buf[_count - 1];
    if (last.comp == comp && strncmp(last.code, code, EVENT_CODE_LEN - 1) == 0) {
      if (last.repeat_count < 65535) last.repeat_count++;
      xSemaphoreGive(_mutex);
      return;
    }
  }

  if (_count >= EVENT_BUFFER_SIZE) {
    // Buffer cheio: descarta o mais antigo e sinaliza a perda. Preferimos
    // perder o começo a perder o que está acontecendo agora.
    memmove(&_buf[0], &_buf[1], sizeof(LogEvent) * (EVENT_BUFFER_SIZE - 1));
    _count = EVENT_BUFFER_SIZE - 1;
    _overflow = true;
  }

  LogEvent& e = _buf[_count];
  e.sev = sev;
  e.comp = comp;
  e.repeat_count = 1;
  strncpy(e.code, code, EVENT_CODE_LEN - 1);
  e.code[EVENT_CODE_LEN - 1] = '\0';
  strncpy(e.msg, msg, EVENT_MSG_LEN - 1);
  e.msg[EVENT_MSG_LEN - 1] = '\0';

  String t = rtc_get_time_str();
  strncpy(e.time, t.c_str(), 5);
  e.time[5] = '\0';

  _count++;
  xSemaphoreGive(_mutex);
}

uint8_t event_log_drain(LogEvent* out, uint8_t max) {
  if (_mutex == nullptr) return 0;
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return 0;

  uint8_t n = _count < max ? _count : max;
  if (n > 0) {
    memcpy(out, _buf, sizeof(LogEvent) * n);
    uint8_t restantes = _count - n;
    if (restantes > 0) memmove(&_buf[0], &_buf[n], sizeof(LogEvent) * restantes);
    _count = restantes;
  }

  xSemaphoreGive(_mutex);
  return n;
}

uint8_t event_log_pending() {
  if (_mutex == nullptr) return 0;
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
  uint8_t n = _count;
  xSemaphoreGive(_mutex);
  return n;
}

bool event_log_take_overflow_flag() {
  if (_mutex == nullptr) return false;
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  bool f = _overflow;
  _overflow = false;
  xSemaphoreGive(_mutex);
  return f;
}
