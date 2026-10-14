// Teste de host de outage_tracker.cpp (o arquivo real, incluído inteiro): o
// resumo que a volta de um período sem contato manda ao servidor — o que
// faltou para explicar o apagão de 5,8 h de 2026-10-13.
#include <stdio.h>
#include <string.h>

#include "outage_tracker.cpp"   // o do firmware, via -I

static int g_fail = 0, g_ok = 0;
#define CHECK(cond, what)                                              \
  do {                                                                 \
    if (cond) { g_ok++; }                                              \
    else { g_fail++; printf("  FALHOU: %s  (linha %d)\n", what, __LINE__); } \
  } while (0)

static bool contem(const char* s, const char* parte) { return strstr(s, parte) != nullptr; }

int main() {
  char msg[120];

  printf("O1 sucesso sem queda antes nao gera resumo\n");
  OutageTracker t;
  CHECK(!t.on_success(1000, 60000, msg, sizeof(msg)), "nada a resumir");

  printf("O2 falha curta (abaixo do minimo) tambem nao\n");
  t.on_failure(2000, -11, -70, true);
  CHECK(t.active(), "periodo aberto");
  CHECK(!t.on_success(5000, 60000, msg, sizeof(msg)), "4 s nao viram evento");
  CHECK(!t.active(), "periodo fechado");

  printf("O3 periodo de 12 min: contado desde o ultimo sucesso, com cada tipo de falha\n");
  // Ultimo sucesso em 5 s (O2). Depois: 3 timeouts, 2 sem conexao, 1 outra.
  t.on_failure(10000, -11, -72, true);
  t.on_failure(20000, -11, -81, true);
  t.on_failure(30000, -11, -76, true);
  t.on_failure(40000, -1, -79, true);
  t.on_failure(50000, -1, 0, false);
  t.on_failure(60000, 500, -74, true);
  CHECK(t.on_success(5000 + 12 * 60000, 60000, msg, sizeof(msg)), "vira resumo");
  printf("   \"%s\"\n", msg);
  CHECK(contem(msg, "Sem contato por 12 min"), "duracao desde o ultimo sucesso");
  CHECK(contem(msg, "6 falhas (3 sem resposta, 2 sem conexao)"), "falhas por tipo");
  CHECK(contem(msg, "RSSI -81..-72"), "faixa de RSSI, ignorando leitura invalida");
  CHECK(!contem(msg, "Wi-Fi"), "sem queda de Wi-Fi, nao fala dela");

  printf("O4 apagao com o Wi-Fi caido: tempo sem Wi-Fi, quedas e motivo\n");
  OutageTracker a;
  a.on_success(0, 60000, msg, sizeof(msg));
  a.on_failure(1000, -11, -63, true);
  a.on_wifi_drop(2000, 200);
  // 5h48 sem Wi-Fi, um ciclo a cada 1 s.
  for (uint32_t ms = 3000; ms <= 3000 + 20880000u; ms += 1000) a.on_no_wifi(ms);
  CHECK(a.on_success(3000 + 20880000u + 5000, 60000, msg, sizeof(msg)), "vira resumo");
  printf("   \"%s\"\n", msg);
  CHECK(contem(msg, "Sem contato por 5h48"), "horas e minutos");
  CHECK(contem(msg, "sem Wi-Fi 5h48, 1 queda(s) motivo 200"), "o que de fato aconteceu");
  CHECK(strlen(msg) < sizeof(msg), "cabe na mensagem do evento");

  printf("O5 ciclos sem Wi-Fi separados por Wi-Fi de pe nao somam o intervalo\n");
  OutageTracker b;
  b.on_success(0, 60000, msg, sizeof(msg));
  b.on_no_wifi(10000);
  b.on_no_wifi(11000);            // +1 s
  b.on_failure(12000, -11, -70, true);
  b.on_no_wifi(100000);           // 88 s depois: outro trecho, nao soma
  b.on_no_wifi(101000);           // +1 s
  CHECK(b.on_success(200000, 60000, msg, sizeof(msg)), "resumo");
  CHECK(contem(msg, "sem Wi-Fi 2 s"), "so os trechos contiguos");

  printf("O6 buffer pequeno nunca estoura\n");
  OutageTracker c;
  c.on_success(0, 60000, msg, sizeof(msg));
  c.on_failure(1000, -11, -70, true);
  c.on_wifi_drop(2000, 201);
  char curto[24];
  CHECK(c.on_success(600000, 60000, curto, sizeof(curto)), "resumo");
  CHECK(strlen(curto) < sizeof(curto), "truncado dentro do buffer");

  printf("\n%d verificacoes ok, %d falharam\n", g_ok, g_fail);
  return g_fail ? 1 : 0;
}
