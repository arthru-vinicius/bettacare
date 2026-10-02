#pragma once
#include "types.h"

/**
 * Botão, tela e LED da placa.
 *
 * Botão (fora da programação):
 *   1×      acende a tela; com ela acesa, passa a página
 *   2×      alimenta agora — dentro do limite de 3 refeições em 24 h, sem exceção
 *   3×      testa o aviso ao peixe (M2), sem ração
 *   segurar 2 s  entra na programação
 *
 * Programação (horários, grãos e automático; editada na RAM):
 *   1× +1 · 3× −1 · 2× próximo campo · segurar grava e sai · 20 s parado cancela
 *
 * Calibração do doseador (entra pelo console, comando `calibrar`):
 *   1× +2° · 3× −2° · 2× alterna repouso/despejo · segurar grava e sai ·
 *   2 min parado cancela
 *
 * Qualquer toque também reconhece o aviso pendente (LED piscando).
 *
 * A tela fica apagada por padrão — o OLED gasta com o uso — e acende 5 s a
 * cada interação, durante a refeição e na programação.
 */
void ui_init();     // depois do Wire.begin()
void ui_update();   // a cada volta do loop

/** `test`: foi o `teste grao` do console, não uma refeição. */
void ui_meal_result(const MealResult& r, bool test);
void ui_denied(DenyReason why, Origin origin);
/** Pelo console: falso se houver algo em andamento. */
bool ui_begin_calibration();
bool ui_calibrating();
/** A tela respondeu no I2C na partida. */
bool ui_has_display();
