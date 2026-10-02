#pragma once
#include "types.h"

/**
 * O que decide se uma refeição sai: ocupado, calibração e o limite de 24 h —
 * e o que faz a agenda rodar, o M1 pulsar todo dia e o principal saber de
 * tudo. Botão, console e enlace só pedem; quem decide é daqui.
 */
void app_init();     // depois de store, clock e link
void app_update();   // a cada volta do loop

/**
 * Pede uma refeição. `grains` 0 = o da agenda. Só `Origin::FORCADO` (o app,
 * de propósito) passa do limite de 24 h; o botão nunca passa.
 */
void app_feed(uint8_t grains, Origin origin);
/** O 3× no botão: só o aviso ao peixe, sem ração. Falso se estiver ocupado. */
bool app_test_warning();

/**
 * Bancada (console). Nenhum deles é refeição: não entram no histórico, no
 * limite de 24 h nem no principal — o `teste grao` despeja um grão de verdade,
 * então é com um copo sob o bico. Falso se estiver ocupado.
 */
bool app_test_grain();
bool app_test_beam(uint16_t seconds);
bool app_test_m1();
/** Leva o slide a `deg`, devagar, e solta o servo — para encaixar o braço na montagem. */
bool app_move_servo(uint8_t deg);
/** Grava a agenda; vale da próxima ocorrência em diante. */
bool app_save_config(const FeederConfig& cfg);
void app_save_calibration(const Calibration& cal);
/** Agenda, calibração e histórico de fábrica, e reinicia. */
void app_factory_reset();

const FeederConfig& app_config();
const Calibration&  app_calibration();
const History&      app_history();
uint8_t             app_meals_24h();
/**
 * Algo para o Arthur ver, até um toque no botão: a última refeição terminou
 * com SENSOR ou VAZIO, ou uma refeição da agenda não saiu (perdida, limite,
 * sem calibração).
 */
bool                app_has_error();
void                app_clear_error();
/** Epoch local da próxima refeição da agenda; 0 = nenhuma (automático desligado ou sem hora). */
uint32_t            app_next_meal_at();
/** Falso: nunca alimentou. */
bool                app_last_feed_age(uint32_t& age_s);
