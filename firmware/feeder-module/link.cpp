#include "link.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <string.h>

#include "config.h"

static char     _linha[LINK_LINE_MAX];
static uint8_t  _len = 0;
static bool     _descartando = false;   // linha longa demais: ignora até o fim
static uint32_t _ultimo_ping = 0;
static bool     _ja_ouviu = false;
static bool     _reconectou = false;

static char    _pendentes[PENDING_LINES][48];
static uint8_t _n_pendentes = 0;

// UART1, roteada para os pinos da UART0. A UART0 é o console do sistema: log
// de erro do ESP-IDF sairia no meio de uma linha do protocolo e a corromperia.
// Com o GPIO21 ligado à UART1, o que o sistema escreve na UART0 não chega ao
// fio — sobra só o log da ROM no boot, que o principal descarta como lixo.
#define LINK Serial1

void link_init() {
  LINK.begin(LINK_BAUD, SERIAL_8N1, PIN_LINK_RX, PIN_LINK_TX);
  // Sem o principal no fio, o RX flutua e capta ruído dos motores. O pull-up
  // segura o nível de repouso da UART; com o principal, quem manda é o TX dele.
  gpio_pullup_en((gpio_num_t)PIN_LINK_RX);
}

bool link_poll(LinkRequest& req) {
  while (LINK.available()) {
    char c = (char)LINK.read();
    if (c == '\0') continue;   // o "break" de quando o fio é plugado
    if (c == '\n' || c == '\r') {
      bool tinha = _len > 0 && !_descartando;
      _linha[_len] = '\0';
      _len = 0;
      bool estava_descartando = _descartando;
      _descartando = false;
      if (!tinha || estava_descartando) continue;
      if (!link_parse(_linha, req)) continue;
      if (req.cmd == LinkCmd::PING) {
        uint32_t agora = millis();
        if (!_ja_ouviu || agora - _ultimo_ping > LINK_TIMEOUT_MS) _reconectou = true;
        _ja_ouviu = true;
        _ultimo_ping = agora;
      }
      return true;
    }
    if (_descartando) continue;
    if (_len < LINK_LINE_MAX - 1) {
      _linha[_len++] = c;
    } else {
      _descartando = true;   // nenhuma linha legítima chega perto disso
    }
  }
  return false;
}

bool link_connected() { return _ja_ouviu && millis() - _ultimo_ping <= LINK_TIMEOUT_MS; }

bool link_take_reconnected() {
  bool r = _reconectou;
  _reconectou = false;
  return r;
}

void link_send(const char* line) { LINK.print(line); }

void link_notify(const char* line) {
  if (link_connected()) {
    link_send(line);
    return;
  }
  // Fila cheia: sai o mais antigo — o mais recente é o que mais importa.
  if (_n_pendentes == PENDING_LINES) {
    memmove(_pendentes[0], _pendentes[1], sizeof(_pendentes[0]) * (PENDING_LINES - 1));
    _n_pendentes--;
  }
  strncpy(_pendentes[_n_pendentes], line, sizeof(_pendentes[0]) - 1);
  _pendentes[_n_pendentes][sizeof(_pendentes[0]) - 1] = '\0';
  _n_pendentes++;
}

void link_flush_pending() {
  for (uint8_t i = 0; i < _n_pendentes; i++) link_send(_pendentes[i]);
  _n_pendentes = 0;
}
