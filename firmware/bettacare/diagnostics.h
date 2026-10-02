#pragma once

#include <Arduino.h>

#include "event_log.h"

/**
 * Autodiagnóstico sob demanda.
 *
 * A diferença para `component_status` do servidor, que é a saúde **passiva**
 * derivada da telemetria que foi chegando: aqui o firmware **sonda
 * ativamente** cada periférico — varre o barramento 1-Wire, procura o DS3231
 * no I²C, escreve e relê uma chave de teste na NVS, lê o ADC. É a diferença
 * entre "o último dado que recebi parecia bom" e "acabei de conferir".
 *
 * ## O que ele deliberadamente NÃO faz
 *
 * Não mexe em atuador nenhum. Não acende a luz, não gira a ventoinha, não
 * muda velocidade. Um diagnóstico que altera o estado do aquário é um
 * diagnóstico que ninguém roda com o bicho dentro — e o valor de um autoteste
 * está justamente em poder rodá-lo a qualquer momento, sem pensar duas vezes.
 *
 * A consequência honesta é que dois componentes só podem ser observados, não
 * testados: a **luminária** (o SSR não tem realimentação — não há como saber
 * se a lâmpada acendeu) e a **ventoinha** quando está parada (sem PWM, o
 * tacômetro em zero é o comportamento correto). Nesses casos o `detail` diz o
 * que foi possível verificar, e `probed` fica falso.
 *
 * ## Onde roda
 *
 * No **núcleo de controle** (`loop()`), acionado por um comando
 * `device.diagnose`. A rodada levou 163 ms medidos em bancada — o grosso é a
 * conversão completa do DS18B20, que pode chegar a 750 ms com um sensor que
 * use o tempo todo do datasheet —, e esse ciclo de loop mais longo não perde
 * toque: o botão é amostrado por um timer próprio (ver `light.cpp`), fora do
 * loop. Antes, com o botão lido pelo loop a 5 Hz, uma pausa dessas seria toque
 * perdido.
 *
 * O relatório fica guardado aqui até a task de rede recolhê-lo no POST
 * seguinte.
 */

static const uint8_t DIAG_MAX_CHECKS = 12;
static const uint8_t DIAG_DETAIL_LEN = 120;

struct DiagnosticCheck {
  EventComponent comp;
  /** Casa com `HEALTH_STATUSES` do contrato: ok/degraded/missing/fault/unknown. */
  char           status[10];
  char           detail[DIAG_DETAIL_LEN];
  /** Falso quando foi leitura passiva, não sondagem — ver o cabeçalho. */
  bool           probed;
};

struct DiagnosticReport {
  bool     valid;
  uint32_t command_id;
  uint32_t ran_at_uptime_ms;
  uint32_t duration_ms;
  uint8_t  count;
  DiagnosticCheck checks[DIAG_MAX_CHECKS];
};

void diagnostics_init();

/**
 * Executa o autoteste completo. Chamado do `loop()` (núcleo 1).
 *
 * @param command_id  o comando que pediu, ou 0 se rodou por conta própria.
 */
void diagnostics_run(uint32_t command_id);

/**
 * Copia o relatório pendente, se houver, e o marca como consumido.
 * Chamado pela task de rede ao montar o corpo do POST.
 *
 * @return false quando não há relatório novo.
 */
bool diagnostics_take(DiagnosticReport& out);
