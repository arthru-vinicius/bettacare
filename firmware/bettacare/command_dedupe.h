#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * Memória dos comandos recentes — para uma reentrega não executar de novo.
 *
 * O servidor reentrega um comando a cada POST até receber o `ack`, por até
 * 15 min (`COMMAND_MAX_AGE_S`). Com o Wi-Fi do aquário perdendo respostas,
 * isso é o que faz o comando chegar; mas a mesma reentrega, sem esta memória,
 * executaria de novo uma alimentação cujo `ack` se perdeu no caminho — ração
 * a mais na água. Estado desejado (`light.set`) é inofensivo repetido;
 * `feeder.feed_now` e `device.reboot` não são.
 *
 * Três respostas possíveis para um `id` que chega:
 *
 *   NEW      primeira vez — executar.
 *   PENDING  já recebido e ainda sem desfecho (na fila do loop, ou
 *            executando) — ignorar: o `ack` sai quando terminar.
 *   DONE     já executado — não executar; confirmar de novo com o mesmo
 *            desfecho, porque o `ack` anterior pode ter se perdido.
 *
 * A memória vive na RAM e se perde num reinício. Para esse caso o servidor
 * tem a regra dele: com `boot_id` novo, não reentrega as ações que não podem
 * ser repetidas (ver `settleAfterReboot` em `apps/server`).
 *
 * Lógica pura, sem FreeRTOS: quem chama segura o mutex. Testada no PC por
 * `firmware/test/test_command_dedupe.cpp`.
 */

static const uint8_t DEDUPE_SLOTS    = 16;
static const uint8_t DEDUPE_CODE_LEN = 32;

enum DedupeVerdict : uint8_t {
  DEDUPE_NEW = 0,
  DEDUPE_PENDING,
  DEDUPE_DONE,
};

class CommandDedupe {
 public:
  void reset();

  /**
   * Registra a chegada de `id`. Em `DEDUPE_DONE`, `ok` e `code` recebem o
   * desfecho guardado (`code` vazio quando `ok`).
   */
  DedupeVerdict receive(uint32_t id, bool* ok, char* code, size_t code_len);

  /** Guarda o desfecho de `id` — chamado junto com cada `ack`. */
  void settle(uint32_t id, bool ok, const char* code);

 private:
  struct Entry {
    uint32_t id;
    bool     done;
    bool     ok;
    char     code[DEDUPE_CODE_LEN];
  };

  Entry   _e[DEDUPE_SLOTS] = {};
  /** Próxima posição a sobrescrever: a memória esquece o mais antigo. */
  uint8_t _next = 0;

  int  _find(uint32_t id) const;
  void _store(uint32_t id, bool done, bool ok, const char* code);
};
