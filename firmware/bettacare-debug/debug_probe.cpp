#include "debug_probe.h"

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>

struct DbgEntry {
  uint32_t ms;
  uint16_t a;
  uint16_t dur;
  uint8_t  kind;
};

static const uint32_t DBG_RING_LEN = 128;
static DbgEntry          _ring[DBG_RING_LEN];
static volatile uint32_t _ring_head = 0;  // total já escrito; a posição é head % LEN
static portMUX_TYPE      _mux = portMUX_INITIALIZER_UNLOCKED;

volatile uint32_t dbg_btn_edges        = 0;
volatile uint32_t dbg_btn_edges_high   = 0;
volatile uint32_t dbg_btn_toggles      = 0;
volatile uint32_t dbg_sched_toggles    = 0;
volatile uint32_t dbg_cmd_toggles      = 0;
volatile uint32_t dbg_ssr_mismatches   = 0;
volatile int      dbg_last_http_status = 0;
volatile uint32_t dbg_http_400         = 0;
volatile int      dbg_wdt_result       = -1;
volatile uint32_t dbg_loop_stack_hwm   = 0;

volatile bool     dbg_temp_enabled     = true;
volatile uint32_t dbg_temp_interval_ms = 5000;
volatile bool     dbg_rtc_enabled      = true;
volatile bool     dbg_feeder_enabled   = true;

volatile uint32_t dbg_rtc_fail_reads   = 0;
volatile uint32_t dbg_rtc_bad_reads    = 0;
volatile uint32_t dbg_rtc_offset_reads = 0;
volatile int32_t  dbg_rtc_offset_min   = 0;
volatile uint32_t dbg_temp_fake_85     = 0;
volatile bool     dbg_diag_request     = false;
volatile bool     dbg_hang_loop        = false;

struct DbgEvent {
  uint32_t ms;
  char     code[32];
  char     msg[80];
};
static const uint32_t DBG_EVENTS_LEN = 16;
static DbgEvent          _events[DBG_EVENTS_LEN];
static volatile uint32_t _events_head = 0;

/** Cópia que não quebra o JSON: aspas e barras viram apóstrofo. */
static void _copy_json_safe(char* dst, const char* src, size_t len) {
  size_t i = 0;
  for (; src[i] != '\0' && i < len - 1; i++) {
    char c = src[i];
    dst[i] = (c == '"' || c == '\\' || (uint8_t)c < 0x20) ? '\'' : c;
  }
  dst[i] = '\0';
}

void dbg_note_event(const char* code, const char* msg) {
  uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
  portENTER_CRITICAL(&_mux);
  DbgEvent& e = _events[_events_head % DBG_EVENTS_LEN];
  e.ms = ms;
  _copy_json_safe(e.code, code, sizeof(e.code));
  _copy_json_safe(e.msg, msg, sizeof(e.msg));
  _events_head = _events_head + 1;
  portEXIT_CRITICAL(&_mux);
}

void IRAM_ATTR dbg_mark_isr(DbgKind kind, uint16_t a) {
  uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
  portENTER_CRITICAL_ISR(&_mux);
  DbgEntry& e = _ring[_ring_head % DBG_RING_LEN];
  e.ms = ms;
  e.a = a;
  e.dur = 0;
  e.kind = kind;
  _ring_head = _ring_head + 1;
  portEXIT_CRITICAL_ISR(&_mux);
}

void dbg_mark(DbgKind kind, uint16_t a, uint16_t dur_ms) {
  uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
  portENTER_CRITICAL(&_mux);
  DbgEntry& e = _ring[_ring_head % DBG_RING_LEN];
  e.ms = ms;
  e.a = a;
  e.dur = dur_ms;
  e.kind = kind;
  _ring_head = _ring_head + 1;
  portEXIT_CRITICAL(&_mux);
}

static const char* _kind_name(uint8_t k) {
  switch (k) {
    case DBG_BTN_EDGE:      return "btn_edge";
    case DBG_BTN_TOGGLE:    return "btn_toggle";
    case DBG_SCHED_TOGGLE:  return "sched_toggle";
    case DBG_CMD_TOGGLE:    return "cmd_toggle";
    case DBG_OW_REQUEST:    return "ow_request";
    case DBG_OW_READ:       return "ow_read";
    case DBG_OW_PROBE:      return "ow_probe";
    case DBG_I2C_READ:      return "i2c_read";
    case DBG_FEEDER_PING:   return "feeder_ping";
    case DBG_POST:          return "post";
    case DBG_SSR_MISMATCH:  return "ssr_mismatch";
    case DBG_BTN_PRESS:     return "btn_press";
    default:                return "?";
  }
}

String dbg_dump_json() {
  static DbgEntry copia[DBG_RING_LEN];
  static DbgEvent eventos[DBG_EVENTS_LEN];
  uint32_t head, ev_head;

  portENTER_CRITICAL(&_mux);
  head = _ring_head;
  memcpy(copia, _ring, sizeof(_ring));
  ev_head = _events_head;
  memcpy(eventos, _events, sizeof(_events));
  portEXIT_CRITICAL(&_mux);

  uint32_t n = head < DBG_RING_LEN ? head : DBG_RING_LEN;
  uint32_t primeiro = head - n;

  String out;
  out.reserve(640 + n * 48 + DBG_EVENTS_LEN * 130);
  out += "{\"now_ms\":";
  out += (uint32_t)(esp_timer_get_time() / 1000);
  out += ",\"btn_edges\":";        out += dbg_btn_edges;
  out += ",\"btn_edges_high\":";   out += dbg_btn_edges_high;
  out += ",\"btn_toggles\":";      out += dbg_btn_toggles;
  out += ",\"sched_toggles\":";    out += dbg_sched_toggles;
  out += ",\"cmd_toggles\":";      out += dbg_cmd_toggles;
  out += ",\"ssr_mismatches\":";   out += dbg_ssr_mismatches;
  out += ",\"last_http\":";        out += dbg_last_http_status;
  out += ",\"http_400\":";         out += dbg_http_400;
  out += ",\"wdt_result\":";       out += dbg_wdt_result;
  out += ",\"reset_reason\":";     out += (int)esp_reset_reason();
  out += ",\"free_heap\":";        out += (uint32_t)esp_get_free_heap_size();
  out += ",\"min_free_heap\":";    out += (uint32_t)esp_get_minimum_free_heap_size();
  out += ",\"max_alloc_heap\":";   out += (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  out += ",\"loop_stack_hwm\":";   out += dbg_loop_stack_hwm;
  out += ",\"temp_enabled\":";     out += dbg_temp_enabled ? "true" : "false";
  out += ",\"temp_interval_ms\":"; out += dbg_temp_interval_ms;
  out += ",\"rtc_enabled\":";      out += dbg_rtc_enabled ? "true" : "false";
  out += ",\"feeder_enabled\":";   out += dbg_feeder_enabled ? "true" : "false";
  out += ",\"rtc_fail_reads\":";   out += dbg_rtc_fail_reads;
  out += ",\"rtc_bad_reads\":";    out += dbg_rtc_bad_reads;
  out += ",\"rtc_offset_reads\":"; out += dbg_rtc_offset_reads;
  out += ",\"rtc_offset_min\":";   out += dbg_rtc_offset_min;
  out += ",\"temp_fake_85\":";     out += dbg_temp_fake_85;

  uint32_t n_ev = ev_head < DBG_EVENTS_LEN ? ev_head : DBG_EVENTS_LEN;
  out += ",\"events\":[";
  for (uint32_t i = 0; i < n_ev; i++) {
    const DbgEvent& e = eventos[(ev_head - n_ev + i) % DBG_EVENTS_LEN];
    if (i > 0) out += ',';
    out += "[";
    out += e.ms;
    out += ",\"";
    out += e.code;
    out += "\",\"";
    out += e.msg;
    out += "\"]";
  }
  out += "]";
  out += ",\"ring_total\":";       out += head;
  out += ",\"ring\":[";
  for (uint32_t i = 0; i < n; i++) {
    const DbgEntry& e = copia[(primeiro + i) % DBG_RING_LEN];
    if (i > 0) out += ',';
    out += "[";
    out += e.ms;
    out += ",\"";
    out += _kind_name(e.kind);
    out += "\",";
    out += e.a;
    out += ',';
    out += e.dur;
    out += ']';
  }
  out += "]}";
  return out;
}
