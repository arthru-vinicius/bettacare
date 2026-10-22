#include "outage_tracker.h"

#include <stdio.h>
#include <string.h>

/** Códigos do HTTPClient do Arduino-ESP32 (HTTPClient.h). */
static const int HTTPC_CONNECTION_REFUSED = -1;
static const int HTTPC_READ_TIMEOUT       = -11;

/**
 * Dois ciclos sem Wi-Fi mais afastados que isto não são o mesmo trecho: entre
 * eles houve Wi-Fi (e uma falha, ou um sucesso), e somar a diferença inteira
 * contaria como "sem Wi-Fi" um tempo em que ele estava de pé.
 */
static const uint32_t NO_WIFI_GAP_MS = 30000;

void OutageTracker::_begin(uint32_t now_ms) {
  if (_active) return;
  _active    = true;
  _start_ms  = _has_last_ok ? _last_ok_ms : now_ms;
  _timeouts  = 0;
  _conn      = 0;
  _other     = 0;
  _last_other = 0;
  _has_heap  = false;
  _no_wifi_ms = 0;
  _last_no_wifi_ms = 0;
  _drops     = 0;
  _last_reason = 0;
  _has_rssi  = false;
}

void OutageTracker::on_failure(uint32_t now_ms, int http_status, int16_t rssi, bool rssi_valid,
                               uint32_t free_heap) {
  _begin(now_ms);
  if (http_status == HTTPC_READ_TIMEOUT) {
    if (_timeouts < 65535) _timeouts++;
  } else if (http_status == HTTPC_CONNECTION_REFUSED) {
    if (_conn < 65535) _conn++;
  } else {
    if (_other < 65535) _other++;
    _last_other = http_status;
  }
  _last_no_wifi_ms = 0;

  if (free_heap > 0 && (!_has_heap || free_heap < _heap_min)) {
    _heap_min = free_heap;
    _has_heap = true;
  }

  if (rssi_valid) {
    if (!_has_rssi) {
      _rssi_min = _rssi_max = rssi;
      _has_rssi = true;
    } else {
      if (rssi < _rssi_min) _rssi_min = rssi;
      if (rssi > _rssi_max) _rssi_max = rssi;
    }
  }
}

void OutageTracker::on_no_wifi(uint32_t now_ms) {
  _begin(now_ms);
  if (_last_no_wifi_ms != 0 && now_ms - _last_no_wifi_ms < NO_WIFI_GAP_MS) {
    _no_wifi_ms += now_ms - _last_no_wifi_ms;
  }
  _last_no_wifi_ms = now_ms;
}

void OutageTracker::on_wifi_drop(uint32_t now_ms, uint8_t reason) {
  _begin(now_ms);
  if (_drops < 65535) _drops++;
  _last_reason = reason;
}

/** "45 s", "12 min", "5h48" — curto, porque a mensagem do evento tem 120 bytes. */
static void _duracao(uint32_t ms, char* out, size_t len) {
  uint32_t s = ms / 1000;
  if (s < 120) {
    snprintf(out, len, "%lu s", (unsigned long)s);
  } else if (s < 7200) {
    snprintf(out, len, "%lu min", (unsigned long)(s / 60));
  } else {
    snprintf(out, len, "%luh%02lu", (unsigned long)(s / 3600), (unsigned long)((s % 3600) / 60));
  }
}

bool OutageTracker::on_success(uint32_t now_ms, uint32_t min_ms, char* out, size_t len) {
  bool tinha = _active;
  uint32_t duracao = now_ms - _start_ms;

  _active = false;
  _last_ok_ms = now_ms;
  _has_last_ok = true;

  if (!tinha || duracao < min_ms || out == nullptr || len == 0) return false;

  // Montado por partes, só com o que aconteceu: um período só de timeouts não
  // precisa dizer "0 sem conexao, sem Wi-Fi 0 s".
  char d[16];
  _duracao(duracao, d, sizeof(d));
  size_t n = (size_t)snprintf(out, len, "Sem contato por %s", d);

  unsigned falhas = (unsigned)_timeouts + _conn + _other;
  if (falhas > 0 && n < len) {
    n += (size_t)snprintf(out + n, len - n, ": %u %s (%u sem resposta, %u sem conexao",
                          falhas, falhas == 1 ? "falha" : "falhas", (unsigned)_timeouts,
                          (unsigned)_conn);
    if (_other > 0 && n < len) {
      // Só o código negativo diz algo (-8 é "sem RAM"); um 5xx já tem a sua história no servidor.
      const char* palavra = _other == 1 ? "outra" : "outras";
      if (_last_other < 0) {
        n += (size_t)snprintf(out + n, len - n, ", %u %s, ult. %d", (unsigned)_other, palavra,
                              _last_other);
      } else {
        n += (size_t)snprintf(out + n, len - n, ", %u %s", (unsigned)_other, palavra);
      }
    }
    if (n < len) n += (size_t)snprintf(out + n, len - n, ")");
  }
  if ((_no_wifi_ms > 0 || _drops > 0) && n < len) {
    char w[16];
    _duracao(_no_wifi_ms, w, sizeof(w));
    n += (size_t)snprintf(out + n, len - n, "; sem Wi-Fi %s, %u queda(s)", w, (unsigned)_drops);
    if (_drops > 0 && n < len) {
      n += (size_t)snprintf(out + n, len - n, " motivo %u", (unsigned)_last_reason);
    }
  }
  if (_has_rssi && n < len) {
    n += (size_t)snprintf(out + n, len - n, "; RSSI %d..%d", (int)_rssi_min, (int)_rssi_max);
  }
  if (_has_heap && n < len) {
    snprintf(out + n, len - n, "; heap min %lu KB", (unsigned long)(_heap_min / 1024));
  }
  return true;
}
