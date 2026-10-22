// Teste de host de net_watchdog.h (o cabeçalho real): quando reassociar o Wi-Fi e
// quando reiniciar o aparelho por falta de contato. A decisão é a parte que
// erra em silêncio — um reinício à toa apaga e acende a luminária, e o
// controlador dela troca de padrão a cada liga-e-desliga rápido.
#include <stdio.h>

#include "net_watchdog.h"   // o do firmware, via -I

static int g_fail = 0, g_ok = 0;
#define CHECK(cond, what)                                              \
  do {                                                                 \
    if (cond) { g_ok++; }                                              \
    else { g_fail++; printf("  FALHOU: %s  (linha %d)\n", what, __LINE__); } \
  } while (0)

static const uint32_t MIN = 60UL * 1000UL;
static const uint32_t H = 60UL * MIN;
static const uint32_t NUNCA = 0xFFFFFFFFUL;

int main() {
  printf("W1 reassociar: so depois de 2 min de POSTs falhando\n");
  CHECK(!net_stall_deve_reassociar(30 * 1000UL, NUNCA), "30 s ainda e perda de pacote comum");
  CHECK(!net_stall_deve_reassociar(NET_STALL_PRAZO_MS - 1, NUNCA), "um ms antes do prazo");
  CHECK(net_stall_deve_reassociar(NET_STALL_PRAZO_MS, NUNCA), "no prazo");
  CHECK(net_stall_deve_reassociar(10 * MIN, NUNCA), "muito depois do prazo");

  printf("W2 reassociar: nunca duas vezes em menos de 5 min\n");
  CHECK(!net_stall_deve_reassociar(10 * MIN, 1 * MIN), "reassociou ha 1 min");
  CHECK(!net_stall_deve_reassociar(10 * MIN, NET_STALL_ESPERA_MS - 1), "um ms antes da espera");
  CHECK(net_stall_deve_reassociar(10 * MIN, NET_STALL_ESPERA_MS), "passada a espera");

  printf("W3 o prazo do reinicio dobra ate o teto de 24 h\n");
  CHECK(net_watchdog_prazo_ms(0) == 2 * H, "primeiro: 2 h");
  CHECK(net_watchdog_prazo_ms(1) == 4 * H, "segundo: 4 h");
  CHECK(net_watchdog_prazo_ms(2) == 8 * H, "terceiro: 8 h");
  CHECK(net_watchdog_prazo_ms(3) == 16 * H, "quarto: 16 h");
  CHECK(net_watchdog_prazo_ms(4) == 24 * H, "quinto: teto de 24 h (32 h cortado)");
  CHECK(net_watchdog_prazo_ms(8) == 24 * H, "sequencia maxima segue no teto");
  CHECK(net_watchdog_prazo_ms(255) == 24 * H, "valor absurdo nao estoura");

  printf("W4 reiniciar: so apos o prazo e com a luminaria apagada\n");
  CHECK(!net_watchdog_deve_reiniciar(2 * H - 1, 0, false, 5 * H), "um ms antes de 2 h");
  CHECK(net_watchdog_deve_reiniciar(2 * H, 0, false, 5 * H), "2 h sem contato, luz apagada");
  CHECK(!net_watchdog_deve_reiniciar(21 * H, 0, true, 30 * H),
        "21 h sem contato, mas a luz esta acesa: espera apagar (reiniciar troca o padrao da luminaria)");
  CHECK(net_watchdog_deve_reiniciar(21 * H, 0, false, 30 * H), "apagou: agora reinicia");

  printf("W5 reiniciar: um boot recem-feito nao tem horas de silencio\n");
  // Sem nenhum POST desde o boot, o silencio e o proprio uptime — que ainda e curto.
  CHECK(!net_watchdog_deve_reiniciar(3 * H, 0, false, 10 * MIN), "uptime de 10 min");
  CHECK(!net_watchdog_deve_reiniciar(3 * H, 0, false, NET_WD_UPTIME_MINIMO_MS - 1), "um ms antes dos 30 min");
  CHECK(net_watchdog_deve_reiniciar(3 * H, 0, false, NET_WD_UPTIME_MINIMO_MS), "30 min de uptime");

  printf("W6 reinicios seguidos esperam cada vez mais\n");
  CHECK(!net_watchdog_deve_reiniciar(3 * H, 1, false, 5 * H), "segundo reinicio: 3 h nao basta (4 h)");
  CHECK(net_watchdog_deve_reiniciar(4 * H, 1, false, 5 * H), "segundo reinicio: 4 h");
  CHECK(!net_watchdog_deve_reiniciar(23 * H, 4, false, 30 * H), "no teto: 23 h nao basta");
  CHECK(net_watchdog_deve_reiniciar(24 * H, 4, false, 30 * H), "no teto: 24 h");

  printf("\n%d verificacoes ok, %d falharam\n", g_ok, g_fail);
  return g_fail ? 1 : 0;
}
