#include "temperature.h"

#include <DallasTemperature.h>
#include <OneWire.h>

#include "config.h"
#include "debug_probe.h"
#include "event_log.h"

#ifndef TEMP_MAX_STALE_MS
#define TEMP_MAX_STALE_MS 15000UL
#endif

static OneWire           _bus(PIN_DS18B20);
static DallasTemperature _sensors(&_bus);
static bool              _available          = false;
static float             _cached_celsius     = NAN;
static bool              _has_valid_reading  = false;
static bool              _conversion_pending = false;
static unsigned long     _conversion_start   = 0;
/** Quando a última conversão foi disparada — base do espaçamento. */
static unsigned long     _last_conversion_ms  = 0;
static unsigned long     _last_valid_ms      = 0;
static unsigned long     _last_probe_ms      = 0;

/** Tempo de conversão a 12 bits (0,0625 °C por passo). */
static const uint16_t CONVERSION_MS = 750;
/** Com que frequência reprocurar o sensor no barramento. */
static const unsigned long PROBE_INTERVAL_MS = 10000;

/**
 * Intervalo mínimo entre conversões (longevidade).
 *
 * Antes, `temperature_update()` disparava uma conversão nova assim que a
 * anterior terminava — ~1 a cada 800 ms, ou **108 mil conversões por dia**.
 * Dois motivos para isso ser demais:
 *
 *   1. A água de um aquário muda de temperatura em **minutos**, não em
 *      milissegundos. Amostrar 75× mais rápido que o fenômeno não acrescenta
 *      informação nenhuma — só ruído do último bit.
 *   2. O DS18B20 **se autoaquece** durante a conversão. Convertendo sem parar,
 *      o próprio sensor aquece e passa a medir a si mesmo em vez da água. É um
 *      erro de medição que a amostragem agressiva *cria*.
 *
 * A 5 s continuamos 12× mais rápidos que o heartbeat de 60 s do histórico, e
 * a telemetria (3 s) nunca fica com dado velho o bastante para importar.
 */
static const unsigned long SAMPLE_INTERVAL_MS = 5000;
/**
 * Quantas conversões seguidas podem falhar antes de declarar o sensor perdido.
 * Uma leitura ruim isolada é ruído de cabo; três seguidas é cabo solto.
 */
static const uint8_t FAILURES_TO_LOST = 3;

/**
 * 85,0 °C exato é o valor de power-on do registrador do DS18B20: o que ele
 * devolve quando reiniciou entre o pedido de conversão e a leitura (queda de
 * alimentação, ruído na linha), ou quando a conversão nem chegou a acontecer.
 * Passa no CRC — o dado é íntegro, só não é temperatura. Num aquário nunca é
 * medição real, e aceito mandaria a ventoinha a 100% e prenderia 30 min de
 * cooldown. Vale como leitura inválida.
 */
static const float POWER_ON_RESET_C = 85.0f;

static uint8_t _consecutive_failures = 0;

static void _probe() {
  _last_probe_ms = millis();
  uint32_t t0 = millis();
  _sensors.begin();
  int count = _sensors.getDeviceCount();
  dbg_mark(DBG_OW_PROBE, (uint16_t)count, (uint16_t)(millis() - t0));
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

  if (!dbg_temp_enabled) return;

  if (!_available) {
    if (agora - _last_probe_ms >= PROBE_INTERVAL_MS) _probe();
    return;
  }

  if (!_conversion_pending) {
    // Espaça as conversões (ver `SAMPLE_INTERVAL_MS`). A primeira leitura do
    // boot não espera: `_last_conversion_ms` começa em zero e o dispositivo
    // precisa saber a temperatura antes de decidir qualquer coisa.
    if (_last_conversion_ms != 0 && agora - _last_conversion_ms < dbg_temp_interval_ms) {
      return;
    }
    uint32_t t0 = millis();
    _sensors.requestTemperatures();     // retorna imediatamente
    dbg_mark(DBG_OW_REQUEST, 0, (uint16_t)(millis() - t0));
    _conversion_pending = true;
    _conversion_start = agora;
    _last_conversion_ms = agora;
    return;
  }

  if (agora - _conversion_start < CONVERSION_MS) return;
  _conversion_pending = false;

  uint32_t t0 = millis();
  float t = _sensors.getTempCByIndex(0);
  dbg_mark(DBG_OW_READ, 0, (uint16_t)(millis() - t0));
  if (dbg_temp_fake_85 > 0) {
    dbg_temp_fake_85 = dbg_temp_fake_85 - 1;
    t = POWER_ON_RESET_C;
  }

  // Conversão exata, sem margem: o DallasTemperature escala o registrador por
  // potência de dois, e 0x0550 vira exatamente 85.0f.
  bool valor_de_reset = (t == POWER_ON_RESET_C);
  if (valor_de_reset) {
    event_log(SEV_WARN, COMP_TEMP, "temp.reset_value",
              "DS18B20 devolveu 85.0C, o valor de power-on; leitura descartada");
  }

  // DEVICE_DISCONNECTED_C é -127; qualquer coisa abaixo de -100 é o sensor
  // dizendo que não está lá, não uma temperatura.
  if (t > -100.0f && !valor_de_reset) {
    _cached_celsius = t;
    _has_valid_reading = true;
    _last_valid_ms = agora;
    _consecutive_failures = 0;
    return;
  }

  if (_consecutive_failures < 255) _consecutive_failures++;
  if (_consecutive_failures == FAILURES_TO_LOST) {
    event_log(SEV_WARN, COMP_TEMP, "temp.crc_error",
              "%u leituras invalidas seguidas; sensor considerado perdido",
              (unsigned)_consecutive_failures);

    /**
     * A transição vira evento **aqui**, não em `_probe()` (UPGRADE/03, F4).
     * `_probe()` só fala quando `encontrado != _available` — e como a linha
     * seguinte já marca `_available = false`, a próxima sondagem que
     * confirme "não está lá" bate `false == _available (false)` e volta
     * calada. Sem isto, `temp.sensor_lost` nunca saía por este caminho: um
     * DS18B20 que morresse por cabo solto emitia um único `temp.crc_error` e
     * depois silêncio — a pergunta "desde quando o sensor sumiu?" ficava sem
     * resposta justamente no caso mais comum.
     */
    _available = false;
    _has_valid_reading = false;
    _cached_celsius = NAN;
    _last_probe_ms = agora;
    event_log(SEV_ERROR, COMP_TEMP, "temp.sensor_lost",
              "DS18B20 parou de responder apos %u leituras invalidas seguidas",
              (unsigned)_consecutive_failures);
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
