#pragma once

#include <Arduino.h>

/**
 * DS18B20 em modo não-bloqueante.
 *
 * A conversão a 12 bits leva ~750 ms. O firmware antigo já não bloqueava
 * esperando por ela, e essa parte é portada como está — é a solução certa.
 *
 * O que muda: o barramento passa a ser **re-sondado periodicamente**. Antes o
 * sensor era procurado só no boot, então um cabo que caísse depois deixava
 * `available` verdadeiro para sempre e as leituras apenas paravam de mudar. O
 * requisito de "saber quando um componente deixou de ser reconhecido" não tem
 * como ser atendido sem isto.
 */
void temperature_init();

/** Chamado a cada ciclo do loop. Gerencia conversão e re-sondagem. */
void temperature_update();

/** Última leitura válida em °C, ou NAN se não houver nenhuma. */
float temperature_read();

/** O sensor está presente no barramento 1-Wire? */
bool temperature_available();

/** Houve ao menos uma leitura válida desde que o sensor foi encontrado? */
bool temperature_has_valid_reading();

/** A última leitura válida é recente o bastante para decidir algo? */
bool temperature_is_fresh();

/** Idade da última leitura válida, em ms. É o que vira `temp.stale`. */
uint32_t temperature_age_ms();
