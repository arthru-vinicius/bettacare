#include "net_watchdog.h"

#include <Arduino.h>
#include <Preferences.h>

#include "event_log.h"

#define WD_NS         "bc_netwd"
#define WD_KEY_SEQ    "seq"
#define WD_KEY_PEND   "pend"
#define WD_KEY_SILMIN "silmin"

/** Teto da sequência: acima disso o prazo já está no teto de 24 h de qualquer jeito. */
static const uint8_t SEQUENCIA_MAXIMA = 8;

static uint8_t  _seq = 0;
/** Este boot veio de um reinício do vigia e o evento ainda não saiu. */
static bool     _pendente = false;
static uint32_t _silencio_min = 0;

void net_watchdog_init() {
  Preferences p;
  if (!p.begin(WD_NS, true)) return;
  _seq = p.getUChar(WD_KEY_SEQ, 0);
  _pendente = p.getBool(WD_KEY_PEND, false);
  _silencio_min = p.getUInt(WD_KEY_SILMIN, 0);
  p.end();
}

uint8_t net_watchdog_sequencia() { return _seq; }

void net_watchdog_reiniciar(uint32_t sem_contato_ms) {
  uint8_t proxima = _seq < SEQUENCIA_MAXIMA ? (uint8_t)(_seq + 1) : _seq;
  Preferences p;
  if (p.begin(WD_NS, false)) {
    p.putUChar(WD_KEY_SEQ, proxima);
    p.putBool(WD_KEY_PEND, true);
    p.putUInt(WD_KEY_SILMIN, sem_contato_ms / 60000UL);
    p.end();
  }
  Serial.printf("[NetWD] %lu min sem contato e luz apagada: reiniciando\n",
                (unsigned long)(sem_contato_ms / 60000UL));
  delay(100);
  ESP.restart();
}

void net_watchdog_contato_ok(uint32_t uptime_ms) {
  if (_pendente) {
    _pendente = false;
    event_log(SEV_WARN, COMP_SYSTEM, "system.net_watchdog_reboot",
              "Reiniciado pelo vigia de rede apos %lu min sem contato",
              (unsigned long)_silencio_min);
    Preferences p;
    if (p.begin(WD_NS, false)) {
      p.putBool(WD_KEY_PEND, false);
      p.end();
    }
  }

  if (_seq > 0 && uptime_ms >= NET_WD_RECUPERACAO_MS) {
    _seq = 0;
    Preferences p;
    if (p.begin(WD_NS, false)) {
      p.putUChar(WD_KEY_SEQ, 0);
      p.end();
    }
  }
}
