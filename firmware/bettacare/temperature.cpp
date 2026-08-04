#include "temperature.h"
#include "config.h"
#include <OneWire.h>
#include <DallasTemperature.h>

#ifndef TEMP_MAX_STALE_MS
#define TEMP_MAX_STALE_MS 5000UL
#endif

static OneWire           _bus(PIN_DS18B20);
static DallasTemperature _sensors(&_bus);
static bool              _available          = false;
static float             _cached_celsius     = -127.0f;
static bool              _has_valid_reading  = false;
static bool              _conversion_pending = false;
static unsigned long     _conversion_start   = 0;
static unsigned long     _last_valid_ms      = 0;

// Tempo de conversão para resolução 12 bits (0,0625°C por passo)
static const uint16_t CONVERSION_MS = 750;

void temperature_init() {
  _sensors.begin();
  _sensors.setWaitForConversion(false);  // modo não-bloqueante

  int count  = _sensors.getDeviceCount();
  _available = (count > 0);

  if (_available) {
    _sensors.setResolution(12);
    Serial.printf("[Temperature] DS18B20 encontrado. Sensores: %d\n", count);
  } else {
    Serial.println("[Temperature] DS18B20 NAO encontrado. Verifique o pino e a conexao.");
  }
}

void temperature_update() {
  if (!_available) return;

  if (!_conversion_pending) {
    // Inicia nova conversão; retorna imediatamente (não-bloqueante)
    _sensors.requestTemperatures();
    _conversion_pending = true;
    _conversion_start   = millis();
    return;
  }

  // Aguarda o tempo mínimo de conversão antes de ler
  if (millis() - _conversion_start < CONVERSION_MS) return;

  float t = _sensors.getTempCByIndex(0);
  if (t > -100.0f) {
    // Leitura válida: atualiza cache
    _cached_celsius = t;
    _has_valid_reading = true;
    _last_valid_ms = millis();
  }
  _conversion_pending = false;
}

float temperature_read() {
  return _cached_celsius;
}

bool temperature_available() {
  return _available;
}

bool temperature_has_valid_reading() {
  return _available && _has_valid_reading;
}

bool temperature_is_fresh() {
  if (!temperature_has_valid_reading()) return false;
  return (millis() - _last_valid_ms) <= TEMP_MAX_STALE_MS;
}
