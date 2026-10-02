#pragma once
#include <stdint.h>

#include "params.h"

/** A agenda. A NVS do módulo é a fonte de verdade; o app só pede mudanças. */
struct FeederConfig {
  uint8_t hour1;
  uint8_t hour2;
  uint8_t grains;
  bool    auto_enabled;
};

/** Ângulos do doseador, calibrados na bancada. */
struct Calibration {
  uint8_t rest_deg;   // bolso sob o reservatório
  uint8_t dump_deg;   // bolso sobre o tubo de queda
  bool    valid;      // falso até a primeira calibração salva
};

/** Quem pediu a refeição — vai no FED e no DENIED, e aparece na tela. */
enum class Origin : uint8_t { AGENDA, RECUP, BOTAO, APP, FORCADO };

/** Como a refeição terminou. */
enum class MealReason : uint8_t { OK, SENSOR, VAZIO };

/** Por que um pedido foi recusado. */
enum class DenyReason : uint8_t { LIMITE, OCUPADO, CALIBRAR, PERDIDA };

/**
 * O que precisa sobreviver a um desligamento: o que já foi servido, as
 * refeições recentes (o limite de 24 h), a última refeição e o último pulso
 * do M1.
 *
 * Horários em "epoch local": segundos desde 1970 contados no fuso do
 * aquário, sem conversão — o módulo só compara horários entre si.
 */
struct History {
  uint32_t served[2];               // ocorrência de cada horário já tratada
  uint32_t meals[MEALS_HISTORY];    // quando foram as últimas refeições; 0 = vazio
  uint8_t  meals_next;              // próxima posição do anel
  uint32_t last_feed_at;            // 0 = nunca
  uint8_t  last_req;
  uint8_t  last_conf;
  bool     last_ok;
  uint8_t  last_reason;             // MealReason
  uint32_t last_m1_at;              // 0 = nunca
};

/** Resultado de uma refeição. */
struct MealResult {
  uint8_t    requested;
  uint8_t    confirmed;   // 0 se o sensor falhou: foi contado pelo servo
  bool       ok;
  MealReason reason;
  Origin     origin;
};
