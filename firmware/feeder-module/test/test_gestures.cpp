// Gestos do botão: 1×, 2×, 3× e o toque longo.
#include "check.h"
#include "gestures.cpp"

static uint32_t t = 1000;
static GestureDetector g;

/** Um toque de 80 ms, e o tempo seguindo. */
static void toque() {
  g.press(t);
  t += 80;
  g.release(t);
}

/** Roda o tempo até `ms` e devolve o primeiro gesto que sair. */
static Gesture espera(uint32_t ms) {
  for (uint32_t i = 0; i < ms; i += 10) {
    t += 10;
    Gesture x = g.update(t);
    if (x != Gesture::NONE) return x;
  }
  return Gesture::NONE;
}

int main() {
  printf("G1 um toque\n");
  toque();
  CHECK(g.update(t + 100) == Gesture::NONE, "ainda na janela");
  CHECK(espera(600) == Gesture::TAP1, "1x");

  printf("G2 dois e tres toques\n");
  toque();
  t += 150;
  toque();
  CHECK(espera(600) == Gesture::TAP2, "2x");
  toque();
  t += 150;
  toque();
  t += 150;
  toque();
  CHECK(espera(600) == Gesture::TAP3, "3x");

  printf("G3 quatro toques contam como tres\n");
  for (int i = 0; i < 4; i++) {
    toque();
    t += 150;
  }
  CHECK(espera(600) == Gesture::TAP3, "3x");

  printf("G4 toques espacados demais sao gestos separados\n");
  toque();
  CHECK(espera(600) == Gesture::TAP1, "primeiro");
  toque();
  CHECK(espera(600) == Gesture::TAP1, "segundo");

  printf("G5 segurar: o longo sai com o dedo ainda no botao, e a soltura nao vira toque\n");
  g.press(t);
  CHECK(espera(1900) == Gesture::NONE, "1,9 s: nada");
  CHECK(espera(200) == Gesture::LONG, "2 s: longo");
  t += 500;
  g.release(t);
  CHECK(espera(600) == Gesture::NONE, "soltura do longo nao e toque");

  printf("G6 toque e depois segurar: vale o longo\n");
  toque();
  t += 100;
  g.press(t);
  CHECK(espera(2100) == Gesture::LONG, "longo");
  t += 100;
  g.release(t);
  CHECK(espera(600) == Gesture::NONE, "o toque de antes foi descartado");

  return resumo();
}
