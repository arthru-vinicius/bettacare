#pragma once
#include <Arduino.h>

/**
 * @brief Inicializa os pinos do relé SSR (OUTPUT) e do push button (INPUT_PULLUP).
 *        Estado inicial: luminária desligada.
 */
void light_init();

/**
 * @brief Liga ou desliga a luminária via relé SSR40DA.
 * @param on true para ligar, false para desligar
 */
void light_set(bool on);

/**
 * @brief Inverte o estado atual da luminária.
 */
void light_toggle();

/**
 * @brief Retorna o estado atual da luminária.
 * @return true se ligada, false se desligada
 */
bool light_get_state();

/**
 * @brief Verifica o push button físico com debounce (50ms).
 *        Deve ser chamado no loop(). Aciona light_toggle() na borda de descida.
 */
void light_check_button();
