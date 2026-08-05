#pragma once

#include <Arduino.h>

#include "app_state.h"

/**
 * Luminária (SSR40DA) e o botão físico.
 *
 * O SSR **não tem realimentação**: não há como o firmware saber se a lâmpada
 * realmente acendeu. A única evidência possível de defeito é a divergência
 * entre o que o servidor comandou e o que o dispositivo reporta, e essa
 * comparação é feita do lado do servidor. Aqui a responsabilidade é registrar
 * fielmente o estado do GPIO e a origem da última mudança.
 */
void light_init();

/** Aplica o estado desejado e registra de onde veio a decisão. */
void light_set(bool on, LightSource source);

bool light_get_state();
LightSource light_get_source();

/**
 * Lê o botão físico com debounce.
 *
 * O botão continua sendo alternância — é um botão, é o que um humano espera
 * dele. O que deixou de existir é o **comando** `light.toggle` vindo pela
 * rede, que era o problema: reenvio de toggle inverte o estado duas vezes.
 */
void light_check_button();

/** Verdadeiro se o botão está preso pressionado há tempo demais. */
bool light_button_stuck();
