#include "command_dedupe.h"

#include <string.h>

void CommandDedupe::reset() {
  memset(_e, 0, sizeof(_e));
  _next = 0;
}

int CommandDedupe::_find(uint32_t id) const {
  // `id` 0 nunca vem do servidor (identidade do PostgreSQL começa em 1), e é
  // o valor das posições vazias — então não pode casar com nada.
  if (id == 0) return -1;
  for (uint8_t i = 0; i < DEDUPE_SLOTS; i++) {
    if (_e[i].id == id) return i;
  }
  return -1;
}

void CommandDedupe::_store(uint32_t id, bool done, bool ok, const char* code) {
  Entry& e = _e[_next];
  e.id   = id;
  e.done = done;
  e.ok   = ok;
  if (code != nullptr && !ok) {
    strncpy(e.code, code, DEDUPE_CODE_LEN - 1);
    e.code[DEDUPE_CODE_LEN - 1] = '\0';
  } else {
    e.code[0] = '\0';
  }
  _next = (uint8_t)((_next + 1) % DEDUPE_SLOTS);
}

DedupeVerdict CommandDedupe::receive(uint32_t id, bool* ok, char* code, size_t code_len) {
  if (id == 0) return DEDUPE_NEW;

  int i = _find(id);
  if (i < 0) {
    _store(id, false, false, nullptr);
    return DEDUPE_NEW;
  }
  if (!_e[i].done) return DEDUPE_PENDING;

  if (ok != nullptr) *ok = _e[i].ok;
  if (code != nullptr && code_len > 0) {
    strncpy(code, _e[i].code, code_len - 1);
    code[code_len - 1] = '\0';
  }
  return DEDUPE_DONE;
}

void CommandDedupe::settle(uint32_t id, bool ok, const char* code) {
  if (id == 0) return;

  int i = _find(id);
  if (i < 0) {
    // Recusado antes de entrar na memória (fila cheia, comando desconhecido)
    // ou já esquecido: guarda assim mesmo, para a reentrega repetir o ack.
    _store(id, true, ok, code);
    return;
  }
  Entry& e = _e[i];
  e.done = true;
  e.ok   = ok;
  if (code != nullptr && !ok) {
    strncpy(e.code, code, DEDUPE_CODE_LEN - 1);
    e.code[DEDUPE_CODE_LEN - 1] = '\0';
  } else {
    e.code[0] = '\0';
  }
}
