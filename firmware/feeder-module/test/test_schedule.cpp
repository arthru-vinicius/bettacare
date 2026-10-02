// Agenda, recuperação de refeição perdida e limite de 24 h — o que decide se o
// peixe come. Inclui o .cpp real.
#include "check.h"
#include "schedule.cpp"
#include "timeutil.cpp"

static uint32_t em(uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi) {
  CivilTime t = {y, mo, d, h, mi, 0};
  return civil_to_epoch(t);
}

static const char* nome(SlotAction a) {
  switch (a) {
    case SlotAction::NONE: return "NONE";
    case SlotAction::FEED: return "FEED";
    case SlotAction::RECOVER: return "RECOVER";
    case SlotAction::MISS: return "MISS";
  }
  return "?";
}

/** Decide e, como o firmware, marca a ocorrência decidida como tratada. */
static SlotDecision tratar(const FeederConfig& cfg, History& h, uint32_t now) {
  SlotDecision d = schedule_decide(cfg, h, now);
  if (d.action != SlotAction::NONE) h.served[d.slot] = d.occurrence;
  return d;
}

int main() {
  FeederConfig cfg = {5, 17, 5, true};
  History h = {};

  printf("A1 ocorrencia: a mais recente ate agora\n");
  CHECK(slot_occurrence(5, em(2026, 10, 2, 5, 0)) == em(2026, 10, 2, 5, 0), "exatamente na hora");
  CHECK(slot_occurrence(5, em(2026, 10, 2, 4, 59)) == em(2026, 10, 1, 5, 0), "um minuto antes: a de ontem");
  CHECK(slot_occurrence(17, em(2026, 10, 2, 23, 0)) == em(2026, 10, 2, 17, 0), "a de hoje");

  printf("A2 primeiro boot: nada perdido, agenda vale da proxima em diante\n");
  uint32_t boot = em(2026, 10, 2, 12, 0);
  schedule_mark_current(cfg, h, boot);
  CHECK(schedule_decide(cfg, h, boot).action == SlotAction::NONE, "nada a fazer ao meio-dia");

  printf("A3 na hora: alimenta uma vez so\n");
  SlotDecision d = tratar(cfg, h, em(2026, 10, 2, 17, 0));
  CHECK(d.action == SlotAction::FEED && d.slot == 1, "17h");
  CHECK(tratar(cfg, h, em(2026, 10, 2, 17, 1)).action == SlotAction::NONE, "17h01: ja servida");
  CHECK(tratar(cfg, h, em(2026, 10, 2, 17, 59)).action == SlotAction::NONE, "17h59: nada");

  printf("A4 ligado as 05h40: ainda e a refeicao normal\n");
  d = tratar(cfg, h, em(2026, 10, 3, 5, 40));
  CHECK(d.action == SlotAction::FEED && d.slot == 0, "FEED");

  printf("A5 desligado das 16h as 19h: recupera a das 17h (proxima as 05h, a 10 h)\n");
  d = tratar(cfg, h, em(2026, 10, 3, 19, 0));
  CHECK(d.action == SlotAction::RECOVER && d.slot == 1, nome(d.action));

  printf("A6 desligado das 04h as 10h: a das 05h perdida (5 h de atraso)\n");
  d = tratar(cfg, h, em(2026, 10, 4, 10, 0));
  CHECK(d.action == SlotAction::MISS && d.slot == 0, nome(d.action));
  CHECK(tratar(cfg, h, em(2026, 10, 4, 10, 1)).action == SlotAction::NONE, "e nao repete");

  printf("A7 recuperar perto da proxima: pula\n");
  FeederConfig perto = {9, 12, 5, true};
  History hp = {};
  schedule_mark_current(perto, hp, em(2026, 10, 2, 8, 0));
  d = tratar(perto, hp, em(2026, 10, 2, 10, 30));   // 9h com 1h30 de atraso, a das 12h a 1h30
  CHECK(d.action == SlotAction::MISS, nome(d.action));

  printf("A8 duas pendentes: a mais antiga primeiro\n");
  History h2 = {};
  schedule_mark_current(cfg, h2, em(2026, 10, 1, 18, 0));   // tratadas: 1/10 05h e 17h
  d = tratar(cfg, h2, em(2026, 10, 2, 18, 0));              // passou o dia desligado
  CHECK(d.action == SlotAction::MISS && d.slot == 0, "05h de hoje: perdida");
  d = tratar(cfg, h2, em(2026, 10, 2, 18, 0));
  CHECK(d.action == SlotAction::RECOVER && d.slot == 1, "17h: recupera");

  printf("A9 horarios iguais: uma refeicao, nao duas\n");
  FeederConfig igual = {8, 8, 5, true};
  History hi = {};
  schedule_mark_current(igual, hi, em(2026, 10, 2, 7, 0));
  CHECK(tratar(igual, hi, em(2026, 10, 2, 8, 0)).action == SlotAction::FEED, "uma");
  CHECK(tratar(igual, hi, em(2026, 10, 2, 8, 1)).action == SlotAction::NONE, "nao duas");

  printf("A10 automatico desligado: a agenda nao alimenta\n");
  FeederConfig off = cfg;
  off.auto_enabled = false;
  History ho = {};
  CHECK(schedule_decide(off, ho, em(2026, 10, 2, 5, 0)).action == SlotAction::NONE, "nada");

  printf("A11 mudar a agenda para agora nao alimenta na hora\n");
  FeederConfig nova = {14, 20, 5, true};
  schedule_mark_current(nova, h, em(2026, 10, 4, 14, 20));
  CHECK(schedule_decide(nova, h, em(2026, 10, 4, 14, 21)).action == SlotAction::NONE, "so na proxima");
  CHECK(schedule_decide(nova, h, em(2026, 10, 4, 20, 0)).action == SlotAction::FEED, "20h sim");

  printf("A12 refeicoes em 24 h\n");
  History hm = {};
  uint32_t base = em(2026, 10, 2, 8, 0);
  history_add_meal(hm, base);
  history_add_meal(hm, base + 4 * 3600);
  history_add_meal(hm, base + 8 * 3600);
  CHECK(meals_in_24h(hm, base + 9 * 3600) == 3, "tres");
  CHECK(meals_in_24h(hm, base + 24 * 3600 + 60) == 2, "a primeira saiu da janela");
  CHECK(meals_in_24h(hm, base - 3600) == 3, "relogio acertado para tras: na duvida, conta");
  for (int i = 0; i < 10; i++) history_add_meal(hm, base + 9 * 3600 + i);
  CHECK(meals_in_24h(hm, base + 10 * 3600) == MEALS_HISTORY, "o anel nao estoura");

  printf("A13 mudar so os graos com a refeicao pendente: ela sai do mesmo jeito\n");
  History hg = {};
  schedule_mark_current(cfg, hg, em(2026, 10, 2, 12, 0));
  FeederConfig mais = cfg;
  mais.grains = 7;
  schedule_apply_change(cfg, mais, hg, em(2026, 10, 2, 17, 0));   // chegou junto com as 17h
  d = tratar(mais, hg, em(2026, 10, 2, 17, 0));
  CHECK(d.action == SlotAction::FEED && d.slot == 1, "17h continua devida");

  printf("A14 trocar um horario: o novo so na proxima, o outro intacto\n");
  History ht = {};
  schedule_mark_current(cfg, ht, em(2026, 10, 2, 12, 0));
  FeederConfig troca = cfg;
  troca.hour1 = 16;   // 05h -> 16h; a das 17h fica
  schedule_apply_change(cfg, troca, ht, em(2026, 10, 2, 17, 0));   // chegou junto com as 17h
  d = tratar(troca, ht, em(2026, 10, 2, 17, 0));
  CHECK(d.action == SlotAction::FEED && d.slot == 1, "17h, que nao mudou, sai");
  CHECK(tratar(troca, ht, em(2026, 10, 2, 17, 1)).action == SlotAction::NONE, "16h nova: so amanha");
  CHECK(tratar(troca, ht, em(2026, 10, 3, 16, 0)).action == SlotAction::FEED, "16h do dia seguinte sai");

  printf("A15 automatico religado depois de dias: nada de refeicao perdida\n");
  History hl = {};
  FeederConfig desligado = cfg;
  desligado.auto_enabled = false;
  schedule_mark_current(desligado, hl, em(2026, 9, 25, 12, 0));
  schedule_apply_change(desligado, cfg, hl, em(2026, 10, 2, 10, 0));
  CHECK(schedule_decide(cfg, hl, em(2026, 10, 2, 10, 0)).action == SlotAction::NONE, "nada as 10h");
  CHECK(tratar(cfg, hl, em(2026, 10, 2, 17, 0)).action == SlotAction::FEED, "e a das 17h sai");

  printf("A16 horarios iguais que se separam: o segundo nao vira perdido\n");
  History hs = {};
  FeederConfig juntos = {8, 8, 5, true};
  schedule_mark_current(juntos, hs, em(2026, 9, 20, 7, 0));
  for (int dia = 20; dia <= 30; dia++) tratar(juntos, hs, em(2026, 9, dia, 8, 0));   // so o slot 0 anda
  FeederConfig separados = {18, 8, 5, true};   // a 1a vai para 18h; a 2a fica nas 08h
  schedule_apply_change(juntos, separados, hs, em(2026, 9, 30, 12, 0));
  CHECK(schedule_decide(separados, hs, em(2026, 9, 30, 12, 0)).action == SlotAction::NONE, "nada pendente");
  CHECK(tratar(separados, hs, em(2026, 9, 30, 18, 0)).action == SlotAction::FEED, "18h sai");
  CHECK(tratar(separados, hs, em(2026, 10, 1, 8, 0)).action == SlotAction::FEED, "08h do dia seguinte sai");

  printf("A17 duas recuperaveis seguidas: so a primeira, a outra colaria nela\n");
  FeederConfig colados = {8, 9, 5, true};
  History hc = {};
  schedule_mark_current(colados, hc, em(2026, 10, 1, 12, 0));
  uint32_t volta = em(2026, 10, 2, 10, 30);   // desligado das 07h as 10h30
  d = tratar(colados, hc, volta);
  CHECK(d.action == SlotAction::RECOVER && d.slot == 0, "08h recuperada");
  history_add_meal(hc, volta);                 // como o firmware: a refeicao entra no historico
  d = tratar(colados, hc, volta);
  CHECK(d.action == SlotAction::MISS && d.slot == 1, "09h: perdida, nao uma segunda dose");

  printf("A18 botao pouco antes: a recuperacao nao vem por cima\n");
  History hb = {};
  schedule_mark_current(cfg, hb, em(2026, 10, 2, 12, 0));
  history_add_meal(hb, em(2026, 10, 2, 16, 0));   // 2x no botao as 16h; desligado das 16h30 as 18h30
  d = tratar(cfg, hb, em(2026, 10, 2, 18, 30));
  CHECK(d.action == SlotAction::MISS && d.slot == 1, "17h: perdida (o peixe comeu as 16h)");
  History hb2 = {};
  schedule_mark_current(cfg, hb2, em(2026, 10, 2, 12, 0));
  history_add_meal(hb2, em(2026, 10, 2, 12, 30));
  d = tratar(cfg, hb2, em(2026, 10, 2, 18, 30));
  CHECK(d.action == SlotAction::RECOVER, "botao as 12h30, 6 h antes: recupera");

  printf("A19 a recuperavel e a da hora juntas: so a da hora\n");
  History hh = {};
  schedule_mark_current(colados, hh, em(2026, 10, 1, 12, 0));
  uint32_t religou = em(2026, 10, 2, 9, 30);   // desligado das 07h as 09h30: 08h atrasada, 09h na hora
  d = tratar(colados, hh, religou);
  CHECK(d.action == SlotAction::MISS && d.slot == 0, "08h: perdida");
  d = tratar(colados, hh, religou);
  CHECK(d.action == SlotAction::FEED && d.slot == 1, "09h: na hora");

  return resumo();
}
