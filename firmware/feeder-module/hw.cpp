#include "hw.h"

#include <Arduino.h>

#include "config.h"
#include "params.h"

// Servo: 50 Hz, 14 bits — 16384 passos em 20 ms, ~1,2 µs cada.
static const uint32_t SERVO_FREQ_HZ = 50;
static const uint8_t  SERVO_RES_BITS = 14;
// Motores: 20 kHz fica acima do audível; 10 bits sobram para rampa suave.
static const uint32_t MOTOR_FREQ_HZ = 20000;
static const uint8_t  MOTOR_RES_BITS = 10;

static bool _servo_attached = false;

static volatile int      _ir_blocked = -1;   // -1 = desarmado
static volatile uint16_t _ir_count = 0;
static volatile uint32_t _ir_last_us = 0;

/**
 * Um grão cortando o feixe leva a saída do LM393 ao nível "bloqueado". Conta
 * a borda para esse nível, e só uma a cada `IR_DEBOUNCE_MS`: o grão caindo
 * pode fazer o comparador trepidar na borda, e isso não pode virar dois.
 */
static void IRAM_ATTR _ir_isr() {
  int bloqueado = _ir_blocked;
  if (bloqueado < 0) return;
  if (digitalRead(PIN_IR_SENSOR) != bloqueado) return;
  uint32_t agora = micros();
  if (agora - _ir_last_us < (uint32_t)IR_DEBOUNCE_MS * 1000UL) return;
  _ir_last_us = agora;
  _ir_count = _ir_count + 1;
}

void hw_init() {
  // Os pull-downs da montagem seguram tudo desligado no reset; daqui em
  // diante quem segura é o firmware — e antes de qualquer outra coisa.
  pinMode(PIN_M1, OUTPUT);
  digitalWrite(PIN_M1, LOW);
  pinMode(PIN_M2, OUTPUT);
  digitalWrite(PIN_M2, LOW);
  pinMode(PIN_IR_LED, OUTPUT);
  digitalWrite(PIN_IR_LED, LOW);
  pinMode(PIN_SERVO, OUTPUT);
  digitalWrite(PIN_SERVO, LOW);
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, HIGH);   // apagado: o LED da placa acende em baixo
  // A saída do LM393 já tem pull-up no próprio módulo; o interno é para o
  // fio solto. Sensor desconectado lê "bloqueado" com o LED apagado e aceso,
  // e o autoteste reprova — flutuando, ele podia passar por acaso e contar
  // ruído como grão.
  pinMode(PIN_IR_SENSOR, INPUT_PULLUP);

  ledcAttach(PIN_M1, MOTOR_FREQ_HZ, MOTOR_RES_BITS);
  ledcWrite(PIN_M1, 0);
  ledcAttach(PIN_M2, MOTOR_FREQ_HZ, MOTOR_RES_BITS);
  ledcWrite(PIN_M2, 0);

  attachInterrupt(digitalPinToInterrupt(PIN_IR_SENSOR), _ir_isr, CHANGE);
}

uint32_t hw_millis() { return millis(); }

void hw_servo_attach() {
  if (_servo_attached) return;
  ledcAttach(PIN_SERVO, SERVO_FREQ_HZ, SERVO_RES_BITS);
  _servo_attached = true;
}

void hw_servo_write_deg(uint8_t deg) {
  if (!_servo_attached) return;
  if (deg > 180) deg = 180;
  uint32_t us = SERVO_US_MIN + (uint32_t)(SERVO_US_MAX - SERVO_US_MIN) * deg / 180;
  uint32_t duty = us * ((1UL << SERVO_RES_BITS) - 1) / (1000000UL / SERVO_FREQ_HZ);
  ledcWrite(PIN_SERVO, duty);
}

void hw_servo_detach() {
  if (!_servo_attached) return;
  ledcDetach(PIN_SERVO);
  pinMode(PIN_SERVO, OUTPUT);
  digitalWrite(PIN_SERVO, LOW);   // sem pulsos, o MG90S fica solto
  _servo_attached = false;
}

static uint32_t _duty(uint8_t pct) {
  if (pct > 100) pct = 100;
  return (uint32_t)pct * ((1UL << MOTOR_RES_BITS) - 1) / 100;
}

void hw_m1_duty(uint8_t pct) { ledcWrite(PIN_M1, _duty(pct)); }
void hw_m2_duty(uint8_t pct) { ledcWrite(PIN_M2, _duty(pct)); }

void hw_ir_led(bool on) { digitalWrite(PIN_IR_LED, on ? HIGH : LOW); }

int hw_ir_level() { return digitalRead(PIN_IR_SENSOR); }

void hw_ir_arm(int blocked_level) {
  noInterrupts();
  _ir_count = 0;
  _ir_blocked = blocked_level;
  interrupts();
}

void hw_ir_disarm() { _ir_blocked = -1; }

uint16_t hw_ir_take_count() {
  noInterrupts();
  uint16_t n = _ir_count;
  _ir_count = 0;
  interrupts();
  return n;
}

void hw_status_led(bool on) { digitalWrite(PIN_STATUS_LED, on ? LOW : HIGH); }
