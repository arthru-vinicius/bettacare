#include "schedule.h"

static const uint32_t DAY = 86400UL;

uint32_t slot_occurrence(uint8_t hour, uint32_t now) {
  uint32_t ocorrencia = now - (now % DAY) + (uint32_t)hour * 3600UL;
  return ocorrencia <= now ? ocorrencia : ocorrencia - DAY;
}

uint32_t seconds_to_next_slot(const FeederConfig& cfg, uint32_t now) {
  // A próxima de um horário é a mais recente mais um dia.
  uint32_t a = slot_occurrence(cfg.hour1, now) + DAY - now;
  uint32_t b = slot_occurrence(cfg.hour2, now) + DAY - now;
  return a < b ? a : b;
}

/** Nenhuma refeição a menos de `gap` de `now`, para trás ou (relógio acertado) para a frente. */
static bool _longe_de_refeicao(const History& h, uint32_t now, uint32_t gap) {
  for (uint8_t i = 0; i < MEALS_HISTORY; i++) {
    uint32_t t = h.meals[i];
    if (t == 0) continue;
    uint32_t distancia = t <= now ? now - t : t - now;
    if (distancia < gap) return false;
  }
  return true;
}

SlotDecision schedule_decide(const FeederConfig& cfg, const History& h, uint32_t now) {
  SlotDecision decisao = {SlotAction::NONE, 0, 0};
  if (!cfg.auto_enabled) return decisao;

  const uint8_t horas[2] = {cfg.hour1, cfg.hour2};
  // Os dois horários iguais são uma refeição só, não duas.
  const uint8_t quantos = cfg.hour1 == cfg.hour2 ? 1 : 2;

  bool alguma_na_hora = false;
  for (uint8_t i = 0; i < quantos; i++) {
    uint32_t ocorrencia = slot_occurrence(horas[i], now);
    if (h.served[i] == ocorrencia) continue;
    uint32_t atraso = now - ocorrencia;
    if (atraso < ON_TIME_S) alguma_na_hora = true;
    if (decisao.action != SlotAction::NONE && ocorrencia >= decisao.occurrence) continue;

    SlotAction acao;
    if (atraso < ON_TIME_S) {
      acao = SlotAction::FEED;
    } else if (atraso < RECOVERY_WINDOW_S && seconds_to_next_slot(cfg, now) > RECOVERY_MIN_GAP_S &&
               _longe_de_refeicao(h, now, RECOVERY_MIN_GAP_S)) {
      // Recuperada é refeição a mais no meio do dia: não cola na próxima nem
      // na anterior (a do botão, ou outra recuperada agora há pouco).
      acao = SlotAction::RECOVER;
    } else {
      acao = SlotAction::MISS;
    }
    decisao = {acao, i, ocorrencia};
  }
  // A outra está na hora agora: recuperar esta colaria as duas. A da hora
  // tem a vez.
  if (decisao.action == SlotAction::RECOVER && alguma_na_hora) decisao.action = SlotAction::MISS;
  return decisao;
}

void schedule_mark_current(const FeederConfig& cfg, History& h, uint32_t now) {
  h.served[0] = slot_occurrence(cfg.hour1, now);
  h.served[1] = slot_occurrence(cfg.hour2, now);
}

void schedule_apply_change(const FeederConfig& before, const FeederConfig& after, History& h, uint32_t now) {
  bool ligou = after.auto_enabled && !before.auto_enabled;
  // Com os dois horários iguais, só o primeiro era acompanhado: o segundo
  // está com a marca velha e não pode virar "perdida" ao se separar.
  bool segundo_solto = before.hour1 == before.hour2;
  if (ligou || after.hour1 != before.hour1) h.served[0] = slot_occurrence(after.hour1, now);
  if (ligou || segundo_solto || after.hour2 != before.hour2) h.served[1] = slot_occurrence(after.hour2, now);
}

uint8_t meals_in_24h(const History& h, uint32_t now) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MEALS_HISTORY; i++) {
    uint32_t t = h.meals[i];
    if (t == 0) continue;
    uint32_t distancia = t <= now ? now - t : t - now;
    if (distancia < DAY) n++;
  }
  return n;
}

void history_add_meal(History& h, uint32_t at) {
  h.meals[h.meals_next] = at;
  h.meals_next = (uint8_t)((h.meals_next + 1) % MEALS_HISTORY);
}
