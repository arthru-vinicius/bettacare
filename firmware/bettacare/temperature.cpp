#include "temperature.h"

#include <DallasTemperature.h>
#include <OneWire.h>

#include "config.h"
#include "event_log.h"

#ifndef TEMP_MAX_STALE_MS
#define TEMP_MAX_STALE_MS 5000UL
#endif

static OneWire           _bus(PIN_DS18B20);
static DallasTemperature _sensors(&_bus);
static bool              _available          = false;
static float             _cached_celsius     = NAN;
static bool              _has_valid_reading  = false;
static bool              _conversion_pending = false;
static unsigned long     _conversion_start   = 0;
static unsigned long     _last_valid_ms      = 0;
static unsigned long     _last_probe_ms      = 0;

/** Tempo de conversão a 12 bits (0,0625 °C por passo). */
static const uint16_t CONVERSION_MS = 750;
/** Com que frequência reprocurar o sensor no barramento. */
static const unsigned long PROBE_INTERVAL_MS = 10000;
/**
 * Quantas conversões seguidas podem falhar antes de declarar o sensor perdido.
 * Uma leitura ruim isolada é ruído de cabo; três seguidas é cabo solto.
 */
static const uint8_t FAILURES_TO_LOST = 3;

static uint8_t _consecutive_failures = 0;

static void _probe() {
  _last_probe_ms = millis();
  _sensors.begin();
  int count = _sensors.getDeviceCount();
  bool encontrado = (count > 0);

  if (encontrado == _available) return;   // sem transição, nada a dizer

  _available = encontrado;
  if (encontrado) {
    _sensors.setResolution(12);
    _consecutive_failures = 0;
    event_log(SEV_INFO, COMP_TEMP, "temp.sensor_found",
              "DS18B20 reconhecido no barramento (%d sensor(es))", count);
  } else {
    _has_valid_reading = false;
    _cached_celsius = NAN;
    event_log(SEV_ERROR, COMP_TEMP, "temp.sensor_lost",
              "Nenhum DS18B20 responde no 1-Wire; verifique o cabo e o pull-up");
  }
}

void temperature_init() {
  _sensors.begin();
  _sensors.setWaitForConversion(false);   // não-bloqueante
  _available = false;
  _probe();
}

void temperature_update() {
  unsigned long agora = millis();

  if (!_available) {
    if (agora - _last_probe_ms >= PROBE_INTERVAL_MS) _probe();
    return;
  }

  if (!_conversion_pending) {
    _sensors.requestTemperatures();     // retorna imediatamente
    _conversion_pending = true;
    _conversion_start = agora;
    return;
  }

  if (agora - _conversion_start < CONVERSION_MS) return;
  _conversion_pending = false;

  float t = _sensors.getTempCByIndex(0);

  // DEVICE_DISCONNECTED_C é -127; qualquer coisa abaixo de -100 é o sensor
  // dizendo que não está lá, não uma temperatura.
  if (t > -100.0f) {
    _cached_celsius = t;
    _has_valid_reading = true;
    _last_valid_ms = agora;
    _consecutive_failures = 0;
    return;
  }

  if (_consecutive_failures < 255) _consecutive_failures++;
  if (_consecutive_failures == FAILURES_TO_LOST) {
    event_log(SEV_WARN, COMP_TEMP, "temp.crc_error",
              "%u leituras invalidas seguidas; reprocurando o sensor",
              (unsigned)_consecutive_failures);
    _available = false;   // força a re-sondagem, que emitirá temp.sensor_lost
    _last_probe_ms = agora;
  }
}

float temperature_read() { return _cached_celsius; }

bool temperature_available() { return _available; }

bool temperature_has_valid_reading() { return _available && _has_valid_reading; }

uint32_t temperature_age_ms() {
  if (!_has_valid_reading) return UINT32_MAX;
  return (uint32_t)(millis() - _last_valid_ms);
}

bool temperature_is_fresh() {
  if (!temperature_has_valid_reading()) return false;
  return temperature_age_ms() <= TEMP_MAX_STALE_MS;
}
