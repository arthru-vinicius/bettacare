#pragma once
#include "types.h"

/**
 * O ciclo físico de uma refeição, como máquina de estados sem espera
 * bloqueante — o loop segue lendo o botão, o enlace e o relógio no meio da
 * dosagem:
 *
 *   aviso (M2 em rampa) → autoteste do sensor → M1 → um movimento do slide por
 *   grão, contando no feixe → slide de volta → servo solto
 *
 * Sensor reprovado no autoteste: a refeição sai contada pelo servo (um
 * movimento por grão, sem repetir) e termina como SENSOR. Três movimentos
 * seguidos sem grão: para, como VAZIO.
 *
 * Fala com o hardware só por `hw.h` — os testes de host trocam por versões
 * falsas.
 */

enum class DoserPhase : uint8_t {
  IDLE, WARNING, SELFTEST, DOSING, FINISHING,   // refeição
  M1_PULSE, WARNING_TEST, BEAM_TEST,            // o resto: sem ração e sem resultado
};

/** Teto do teste do feixe. */
static const uint16_t BEAM_TEST_MAX_S = 60;

/**
 * A calibração em vigor. Diz onde o slide está depois do boot: o servo não
 * informa a posição, e todo movimento termina no repouso.
 */
void doser_set_calibration(const Calibration& cal);

/** Começa uma refeição. Ignorado se já houver algo em andamento. */
void doser_begin_meal(uint8_t grains, Origin origin, const Calibration& cal);
/** Só o aviso ao peixe, sem ração — o 3× no botão. */
void doser_begin_warning_test();
/** Um pulso do M1, o anti-empacamento diário. */
void doser_begin_m1_pulse(uint16_t ms);
/**
 * Bancada: o autoteste do sensor e, se ele passar, `seconds` contando o que
 * cortar o feixe — grãos passados à mão, para alinhar o par IR. Sem servo e
 * sem motor.
 */
void doser_begin_beam_test(uint16_t seconds);

/**
 * Calibração e console: leva o slide a `deg` na mesma velocidade da refeição
 * — nada de tranco, nem no ajuste — e o segura lá. Só com o doseador parado.
 */
bool doser_servo_hold(uint8_t deg);
/** Como `doser_servo_hold`, mas solta o servo quando o slide chega e assenta. */
bool doser_servo_park(uint8_t deg);

/** A cada volta do loop. */
void doser_update();
/** Estado seguro agora (OTA, emergência): tudo desligado, sem resultado. */
void doser_abort();

bool       doser_busy();
DoserPhase doser_phase();
uint8_t    doser_requested();
uint8_t    doser_confirmed();
/** O sensor passou no último autoteste? Antes do primeiro, verdadeiro. */
bool       doser_sensor_ok();
bool       doser_sensor_tested();
/** O que cortou o feixe no teste do feixe (o atual ou o último). */
uint16_t   doser_beam_count();
/** Verdadeiro uma vez, quando uma refeição termina. */
bool       doser_take_result(MealResult& out);
