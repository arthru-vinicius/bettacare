#pragma once
#include <stdint.h>

/**
 * Tudo o que o doseador toca no hardware, numa fronteira só. O firmware usa
 * `hw.cpp`; os testes de host trocam por versões falsas e conferem o que o
 * doseador mandou fazer — sem servo, sem motor, sem sensor.
 */

/** Atuadores desligados e entradas configuradas. A primeira coisa do `setup()`. */
void     hw_init();
uint32_t hw_millis();

/** Servo: PWM só enquanto se move. Solto, ele não "caça" posição nem esquenta. */
void     hw_servo_attach();
void     hw_servo_write_deg(uint8_t deg);
void     hw_servo_detach();

/** M1 e M2 em porcentagem de PWM (0 = desligado). */
void     hw_m1_duty(uint8_t pct);
void     hw_m2_duty(uint8_t pct);

/** LED infravermelho da barreira — aceso só na dosagem e no autoteste. */
void     hw_ir_led(bool on);
/** Nível da saída do LM393 agora. */
int      hw_ir_level();
/** Passa a contar bordas para o nível "bloqueado" (o que o feixe mostra com o LED apagado). */
void     hw_ir_arm(int blocked_level);
void     hw_ir_disarm();
/** Grãos contados desde a última leitura (e zera). */
uint16_t hw_ir_take_count();

/** LED da placa (no GPIO8, aceso em nível baixo). */
void     hw_status_led(bool on);
