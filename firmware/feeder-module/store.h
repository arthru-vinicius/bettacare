#pragma once
#include "types.h"

/**
 * NVS do módulo — a fonte de verdade da agenda. Cada bloco (agenda,
 * calibração, histórico) é gravado inteiro numa chave só: uma queda de
 * energia no meio deixa o bloco anterior, nunca metade de um e metade de
 * outro. Na leitura, faixa conferida; fora dela, o padrão de fábrica.
 *
 * Grava pouco, de propósito: na mudança de agenda, no começo e no fim de
 * cada refeição e no pulso diário do M1 — umas dez escritas por dia.
 */
void store_init();

FeederConfig store_load_config();
void store_save_config(const FeederConfig& cfg);

Calibration store_load_calibration();
void store_save_calibration(const Calibration& cal);

/** Falso na primeira vez (nada gravado ainda): o histórico vem zerado. */
bool store_load_history(History& h);
void store_save_history(const History& h);

/** Agenda, calibração e histórico de volta ao de fábrica. */
void store_factory_reset();
