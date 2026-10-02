#pragma once
#include "gestures.h"

/**
 * O botão do painel, amostrado por timer a cada 2 ms: pressão depois de 30 ms
 * seguidos em baixo, soltura depois de 30 ms em alto — o mesmo esquema do
 * botão da luz no ESP32 principal. Interrupção por borda aceitava como toque
 * os picos que fios vizinhos induziam (UPGRADE/07); aqui, os motores e o servo
 * são fontes de ruído ainda piores.
 */
void button_init();

/** Passa as pressões e solturas confirmadas ao detector de gestos. */
void button_poll(GestureDetector& g);
