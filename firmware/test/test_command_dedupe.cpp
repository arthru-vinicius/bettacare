// Teste de host de command_dedupe.cpp (o arquivo real, incluído inteiro): a
// reentrega de um comando — que o servidor faz a cada POST até o ack, por até
// 15 min — não pode executar de novo uma alimentação ou um reinício.
#include <stdio.h>
#include <string.h>

#include "command_dedupe.cpp"   // o do firmware, via -I

static int g_fail = 0, g_ok = 0;
#define CHECK(cond, what)                                              \
  do {                                                                 \
    if (cond) { g_ok++; }                                              \
    else { g_fail++; printf("  FALHOU: %s  (linha %d)\n", what, __LINE__); } \
  } while (0)

int main() {
  CommandDedupe d;
  d.reset();
  bool ok = false;
  char code[DEDUPE_CODE_LEN];

  printf("D1 primeira chegada executa; a segunda, antes do desfecho, espera\n");
  CHECK(d.receive(91, &ok, code, sizeof(code)) == DEDUPE_NEW, "91 novo");
  CHECK(d.receive(91, &ok, code, sizeof(code)) == DEDUPE_PENDING,
        "91 ainda na fila do loop: nao executa de novo");

  printf("D2 depois do ack, a reentrega devolve o mesmo desfecho sem executar\n");
  d.settle(91, true, nullptr);
  ok = false;
  strcpy(code, "lixo");
  CHECK(d.receive(91, &ok, code, sizeof(code)) == DEDUPE_DONE, "91 ja executado");
  CHECK(ok && code[0] == '\0', "ok, sem codigo");

  printf("D3 uma recusa volta recusada, com o mesmo codigo\n");
  CHECK(d.receive(92, &ok, code, sizeof(code)) == DEDUPE_NEW, "92 novo");
  d.settle(92, false, "feeder.module_offline");
  CHECK(d.receive(92, &ok, code, sizeof(code)) == DEDUPE_DONE, "92 ja resolvido");
  CHECK(!ok && strcmp(code, "feeder.module_offline") == 0, "mesma recusa");

  printf("D4 recusado antes de entrar (fila cheia) tambem e lembrado\n");
  d.settle(93, false, "cmd.queue_full");
  CHECK(d.receive(93, &ok, code, sizeof(code)) == DEDUPE_DONE, "93 nao vira novo");
  CHECK(!ok && strcmp(code, "cmd.queue_full") == 0, "com o codigo da recusa");

  printf("D5 id 0 nunca casa com posicao vazia\n");
  CommandDedupe vazio;
  vazio.reset();
  CHECK(vazio.receive(0, &ok, code, sizeof(code)) == DEDUPE_NEW, "0 e sempre novo");
  CHECK(vazio.receive(0, &ok, code, sizeof(code)) == DEDUPE_NEW, "e nao fica registrado");

  printf("D6 a memoria esquece o mais antigo depois de %u comandos\n", (unsigned)DEDUPE_SLOTS);
  CommandDedupe m;
  m.reset();
  for (uint32_t id = 100; id < 100 + DEDUPE_SLOTS; id++) {
    m.receive(id, &ok, code, sizeof(code));
    m.settle(id, true, nullptr);
  }
  CHECK(m.receive(100, &ok, code, sizeof(code)) == DEDUPE_DONE, "o mais antigo ainda lembrado");
  // `receive(100)` acima nao ocupa posicao nova; o proximo id novo ocupa.
  m.receive(500, &ok, code, sizeof(code));
  CHECK(m.receive(100, &ok, code, sizeof(code)) == DEDUPE_NEW, "esquecido depois de mais um");
  CHECK(m.receive(115, &ok, code, sizeof(code)) == DEDUPE_DONE, "o mais novo continua");

  printf("D7 codigo longo e truncado, nunca estoura\n");
  CommandDedupe t;
  t.reset();
  t.receive(7, &ok, code, sizeof(code));
  t.settle(7, false, "um.codigo.bem.mais.longo.que.o.buffer.de.trinta.e.dois");
  char curto[8];
  CHECK(t.receive(7, &ok, curto, sizeof(curto)) == DEDUPE_DONE, "resolvido");
  CHECK(strlen(curto) == sizeof(curto) - 1, "cortado no tamanho do destino");

  printf("\n%d verificacoes ok, %d falharam\n", g_ok, g_fail);
  return g_fail ? 1 : 0;
}
