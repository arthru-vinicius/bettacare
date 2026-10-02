#pragma once
#include "types.h"

/**
 * A agenda e o limite de 24 h — lógica pura, sem relógio nem NVS: recebe o
 * "agora" e o histórico, e diz o que fazer. É o que decide se o peixe come,
 * então é o que os testes de host mais cobrem.
 */

enum class SlotAction : uint8_t {
  NONE,
  FEED,      // dentro da hora marcada
  RECOVER,   // atrasada até 4 h, e nenhuma refeição a menos de 4 h — nem a próxima, nem a anterior
  MISS,      // atrasada demais, ou a recuperação cairia perto de outra refeição
};

struct SlotDecision {
  SlotAction action;
  uint8_t    slot;         // 0 = hour1, 1 = hour2
  uint32_t   occurrence;   // a ocorrência em questão (epoch local)
};

/** A ocorrência mais recente do horário `hour` até `now`. */
uint32_t slot_occurrence(uint8_t hour, uint32_t now);

/** Segundos até a próxima ocorrência de qualquer horário da agenda. */
uint32_t seconds_to_next_slot(const FeederConfig& cfg, uint32_t now);

/**
 * O que a agenda pede agora. Com mais de uma pendente, a mais antiga primeiro;
 * quem chama marca a decidida como tratada e pergunta de novo.
 */
SlotDecision schedule_decide(const FeederConfig& cfg, const History& h, uint32_t now);

/**
 * Dá as ocorrências atuais por tratadas: uma agenda nova vale da próxima em
 * diante. Sem isto, mudar o horário para "agora" alimentaria na hora, e o
 * primeiro boot contaria como "perdidas" refeições de antes de o módulo
 * existir.
 */
void schedule_mark_current(const FeederConfig& cfg, History& h, uint32_t now);

/**
 * Agenda mudada com o módulo já rodando: a nova vale da próxima ocorrência em
 * diante, mas só no que mudou — horário trocado, ou o automático que acabou
 * de ser ligado (sem isto, dias desligado virariam refeições "perdidas"). Um
 * horário que não mudou guarda a refeição pendente: mudar só os grãos na hora
 * de uma refeição não pode fazê-la sumir.
 */
void schedule_apply_change(const FeederConfig& before, const FeederConfig& after, History& h, uint32_t now);

/**
 * Refeições a menos de 24 h de `now`. Conta também as do "futuro" — o relógio
 * pode ter sido acertado para trás —, porque na dúvida o limite pesa a favor
 * do peixe.
 */
uint8_t meals_in_24h(const History& h, uint32_t now);

void history_add_meal(History& h, uint32_t at);
