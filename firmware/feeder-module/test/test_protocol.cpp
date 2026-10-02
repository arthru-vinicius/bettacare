// Enlace com o principal, lado do módulo: o que entra é conferido campo a
// campo; o que sai bate com o que o principal (feeder_link.cpp) entende.
#include <string.h>

#include "check.h"
#include "protocol.cpp"

static bool ok(const char* linha, LinkRequest& r) { return link_parse(linha, r); }

int main() {
  LinkRequest r;

  printf("P1 PING\n");
  CHECK(ok("PING", r) && r.cmd == LinkCmd::PING, "ping");
  CHECK(!ok("PING 1", r), "ping com sobra: lixo");

  printf("P2 FEED nas quatro formas\n");
  CHECK(ok("FEED", r) && r.cmd == LinkCmd::FEED && r.grains == 0 && !r.force, "FEED");
  CHECK(ok("FEED 4", r) && r.grains == 4 && !r.force, "FEED 4");
  CHECK(ok("FEED FORCE", r) && r.grains == 0 && r.force, "FEED FORCE");
  CHECK(ok("FEED 4 FORCE", r) && r.grains == 4 && r.force, "FEED 4 FORCE");

  printf("P3 FEED fora da faixa ou com lixo: descartado\n");
  CHECK(!ok("FEED 0", r), "0 graos");
  CHECK(!ok("FEED 21", r), "21 graos");
  CHECK(!ok("FEED 4x", r), "digito corrompido");
  CHECK(!ok("FEED 4 FORCE X", r), "sobra");
  CHECK(!ok("FEED FORCE 4", r), "ordem trocada");

  printf("P4 CFG\n");
  CHECK(ok("CFG 7 19 4 1", r) && r.cmd == LinkCmd::CFG && r.cfg.hour1 == 7 && r.cfg.hour2 == 19 &&
            r.cfg.grains == 4 && r.cfg.auto_enabled,
        "valida");
  CHECK(!ok("CFG 24 19 4 1", r), "hora 24");
  CHECK(!ok("CFG 7 19 0 1", r), "0 graos");
  CHECK(!ok("CFG 7 19 4 2", r), "auto 2");
  CHECK(!ok("CFG 7 19 4", r), "campo faltando");
  CHECK(ok("CFG 07 19 04 1", r) && r.cfg.hour1 == 7 && r.cfg.grains == 4, "zeros a esquerda");
  CHECK(!ok("CFG 7 19 4 1 9", r), "campo sobrando");

  printf("P5 lixo\n");
  CHECK(!ok("", r), "vazia");
  CHECK(!ok("PONG 1 2 3", r), "linha que e do outro lado");
  char longa[200];
  memset(longa, '9', sizeof(longa) - 1);
  longa[sizeof(longa) - 1] = '\0';
  CHECK(!ok(longa, r), "linha gigante");

  printf("P6 o que sai\n");
  char b[LINK_LINE_MAX];
  FeederConfig cfg = {5, 17, 5, true};
  link_format_pong(b, sizeof(b), cfg, UINT32_MAX, 0, 0, false, 0);
  CHECK(strcmp(b, "PONG 5 17 5 1 -1 0 0 0 0\n") == 0, "PONG nunca alimentou");
  link_format_pong(b, sizeof(b), cfg, 3600, 5, 4, false, 2);
  CHECK(strcmp(b, "PONG 5 17 5 1 3600 5 4 0 2\n") == 0, "PONG com historico");
  link_format_schedule(b, sizeof(b), cfg);
  CHECK(strcmp(b, "SCHEDULE 5 17 5 1\n") == 0, "SCHEDULE");
  MealResult m = {5, 0, false, MealReason::VAZIO, Origin::AGENDA};
  link_format_fed(b, sizeof(b), m);
  CHECK(strcmp(b, "FED 5 0 0 VAZIO AGENDA\n") == 0, "FED vazio");
  m = {5, 5, true, MealReason::OK, Origin::FORCADO};
  link_format_fed(b, sizeof(b), m);
  CHECK(strcmp(b, "FED 5 5 1 OK FORCADO\n") == 0, "FED forcado");
  link_format_denied(b, sizeof(b), DenyReason::LIMITE, 3, Origin::BOTAO);
  CHECK(strcmp(b, "DENIED LIMITE 3 BOTAO\n") == 0, "DENIED");
  CHECK(link_format_denied(b, 8, DenyReason::LIMITE, 3, Origin::BOTAO) == 0, "nao cabe: 0, sem cortar");

  return resumo();
}
