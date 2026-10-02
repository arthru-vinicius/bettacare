#pragma once
#include <stdint.h>

/** Só o construtor: o barramento de verdade está no stub do DallasTemperature. */
class OneWire {
 public:
  explicit OneWire(uint8_t) {}
};
