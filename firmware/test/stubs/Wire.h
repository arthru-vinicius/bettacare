// I²C falso: dois registradores do DS3231 (minutos, horas) e um erro programável.
#pragma once
#include <stdint.h>
#include <stddef.h>

struct FakeWire {
  uint8_t end_error = 0;     // o que endTransmission() devolve (0 = ok)
  uint8_t bytes_back = 2;    // quantos bytes requestFrom() entrega
  uint8_t reg_min = 0x00;
  uint8_t reg_hour = 0x00;
  int idx = 0;
  int transactions = 0;
  void begin() {}
  void beginTransmission(uint8_t) { transactions++; }
  size_t write(uint8_t) { return 1; }
  uint8_t endTransmission() { return end_error; }
  uint8_t requestFrom(uint8_t, uint8_t) {
    idx = 0;
    return end_error ? 0 : bytes_back;
  }
  int read() { return idx++ == 0 ? reg_min : reg_hour; }
};
extern FakeWire Wire;
