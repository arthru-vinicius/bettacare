#include "event_log.h"

#include <stdarg.h>
#include <string.h>

#include "rtc_manager.h"

static LogEvent          _buf[EVENT_BUFFER_SIZE];
static uint8_t           _count    = 0;   // eventos válidos, do mais antigo ao mais novo
static bool              _overflow = false;
/** Perdidos por estouro desde o boot — vai no `diag`, ver `event_log.h`. */
static uint16_t          _dropped_total = 0;
static SemaphoreHandle_t _mutex    = nullptr;

/** Quantas entradas recentes a dedup varre (UPGRADE/03, F9). */
static const uint8_t DEDUP_SCAN_BACK = 4;

static const char* const SEV_NAMES[] = {"debug", "info", "warn", "error", "fatal"};

static const char* const COMP_NAMES[] = {
    "system", "wifi", "api", "rtc", "temp", "fan",
    "light",  "button", "pot", "nvs", "ota", "feeder",
};

void event_log_init() {
  _mutex = xSemaphoreCreateMutex();
  _count = 0;
  _overflow = false;
  _dropped_total = 0;
  memset(_buf, 0, sizeof(_buf));
}

const char* event_severity_name(EventSeverity sev) {
  return SEV_NAMES[(uint8_t)sev <= SEV_FATAL ? (uint8_t)sev : (uint8_t)SEV_INFO];
}

const char* event_component_name(EventComponent comp) {
  return COMP_NAMES[(uint8_t)comp <= COMP_FEEDER ? (uint8_t)comp : (uint8_t)COMP_SYSTEM];
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

  /**
   * Deduplicação: mesmo componente e código numa das últimas
   * `DEDUP_SCAN_BACK` entradas vira contador, não linha nova (UPGRADE/03,
   * F9). Comparar só com a última entrada colapsava apenas repetição
   * imediata — um componente oscilando raramente repete: ele alterna
   * (`temp.sensor_lost`, `temp.sensor_found`, `temp.sensor_lost`…), e cada
   * alternância quebrava a dedupe e enchia o buffer de 24 posições em
   * segundos, descartando por estouro justamente o começo do episódio.
   */
  uint8_t varrer_ate = _count > DEDUP_SCAN_BACK ? _count - DEDUP_SCAN_BACK : 0;
  for (uint8_t i = _count; i-- > varrer_ate;) {
    LogEvent& anterior = _buf[i];
    if (anterior.comp == comp && strncmp(anterior.code, code, EVENT_CODE_LEN - 1) == 0) {
      if (anterior.repeat_count < 65535) anterior.repeat_count++;
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
    if (_dropped_total < 65535) _dropped_total++;
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

uint8_t event_log_peek(LogEvent* out, uint8_t max) {
  if (_mutex == nullptr) return 0;
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return 0;

  uint8_t n = _count < max ? _count : max;
  if (n > 0) memcpy(out, _buf, sizeof(LogEvent) * n);

  xSemaphoreGive(_mutex);
  return n;
}

/** Remove os `n` primeiros do buffer. Só pode ser chamada segurando `_mutex`. */
static void _remove_front_locked(uint8_t n) {
  if (n >= _count) {
    _count = 0;
    return;
  }
  memmove(&_buf[0], &_buf[n], sizeof(LogEvent) * (_count - n));
  _count -= n;
}

void event_log_confirm_sent(uint8_t n) {
  if (_mutex == nullptr || n == 0) return;
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  _remove_front_locked(n);
  xSemaphoreGive(_mutex);
}

void event_log_release_sent(uint8_t n) {
  if (_mutex == nullptr || n == 0) return;
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

  if (n > _count) n = _count;

  // Compacta os `n` primeiros em cima de si mesmos, mantendo só os graves.
  // Os que vieram depois (registrados enquanto o POST acontecia) não são
  // tocados: eles nunca chegaram a sair.
  uint8_t mantidos = 0;
  for (uint8_t i = 0; i < n; i++) {
    if (_buf[i].sev >= SEV_ERROR) {
      if (mantidos != i) _buf[mantidos] = _buf[i];
      mantidos++;
    }
  }

  uint8_t descartados = n - mantidos;
  if (descartados > 0) {
    uint8_t restantes = _count - n;
    if (restantes > 0) {
      memmove(&_buf[mantidos], &_buf[n], sizeof(LogEvent) * restantes);
    }
    _count -= descartados;
  }

  xSemaphoreGive(_mutex);
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

uint16_t event_log_dropped_count() { return _dropped_total; }
