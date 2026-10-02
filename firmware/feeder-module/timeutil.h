#pragma once
#include <stdint.h>

/**
 * Datas sem biblioteca: o "epoch local" do módulo (segundos desde 1970 no fuso
 * do aquário) para dia, mês, ano e hora, e de volta. Algoritmo de dias civis
 * do Howard Hinnant — exato para o calendário gregoriano inteiro.
 */

struct CivilTime {
  uint16_t year;
  uint8_t  month;   // 1..12
  uint8_t  day;     // 1..31
  uint8_t  hour;
  uint8_t  minute;
  uint8_t  second;
};

uint32_t civil_to_epoch(const CivilTime& t);
CivilTime epoch_to_civil(uint32_t epoch);
/** Data e hora válidas (ano 2024..2099, dia que existe naquele mês). */
bool civil_valid(const CivilTime& t);
