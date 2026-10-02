#include "temperature.h"

#include <DallasTemperature.h>
#include <OneWire.h>
#include <math.h>

#include "config.h"
#include "debug_probe.h"
#include "event_log.h"

#ifndef TEMP_MAX_STALE_MS
#define TEMP_MAX_STALE_MS 15000UL
#endif

/**
 * Faixa fisicamente possível da água **nesta instalação** — aquário em
 * Recife, com ar-condicionado: nunca abaixo de 10 °C, nunca acima de 45 °C.
 * Fora dela, a leitura é defeito, não temperatura. A mesma faixa está no
 * contrato (`TEMP_PLAUSIBLE_C`), que o servidor usa para conferir de novo.
 * Outra instalação ajusta no `config.h`.
 */
#ifndef TEMP_PLAUSIBLE_MIN_C
#define TEMP_PLAUSIBLE_MIN_C 10.0f
#endif
#ifndef TEMP_PLAUSIBLE_MAX_C
#define TEMP_PLAUSIBLE_MAX_C 45.0f
#endif

static OneWire           _bus(PIN_DS18B20);
static DallasTemperature _sensors(&_bus);
/** Endereço do sensor, lido uma vez na sondagem — não a cada conversão. */
static DeviceAddress     _addr;
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

/**
 * Salto entre duas conversões (5 s) acima disto só vale se a releitura
 * confirmar. A água não muda 2 °C em 5 s; um quadro corrompido muda.
 *
 * O CRC-8 do DS18B20 deixa passar, por acaso, 1 em cada 256 quadros
 * corrompidos. Em produção foi um -48,00 °C entre leituras de 27,13 °C —
 * fora da faixa, e pego por ela. Este filtro pega o caso que a faixa não pega:
 * um quadro corrompido que caia, por exemplo, em 20 °C.
 */
static const float JUMP_CONFIRM_C = 2.0f;
/** A releitura confirma o salto se cair a até isto do valor suspeito. */
static const float CONFIRM_TOLERANCE_C = 0.5f;

static uint8_t _consecutive_failures = 0;
static bool    _jump_pending = false;
static float   _jump_value = NAN;

static void _probe() {
  _last_probe_ms = millis();
  uint32_t t0 = millis();
  _sensors.begin();
  int count = _sensors.getDeviceCount();
  dbg_mark(DBG_OW_PROBE, (uint16_t)count, (uint16_t)(millis() - t0));
  bool encontrado = (count > 0) && _sensors.getAddress(_addr, 0);

  if (encontrado == _available) return;   // sem transição, nada a dizer

  _available = encontrado;
  if (encontrado) {
    _sensors.setResolution(12);
    _consecutive_failures = 0;
    _jump_pending = false;
    event_log(SEV_INFO, COMP_TEMP, "temp.sensor_found",
              "DS18B20 reconhecido no barramento (%d sensor(es))", count);
  } else {
    _has_valid_reading = false;
    _cached_celsius = NAN;
    event_log(SEV_ERROR, COMP_TEMP, "temp.sensor_lost",
              "Nenhum DS18B20 responde no 1-Wire; verifique o cabo e o pull-up");
  }
}

/** Uma conversão sem leitura aproveitável. Três seguidas, o sensor está perdido. */
static void _count_failure(unsigned long agora) {
  if (_consecutive_failures < 255) _consecutive_failures++;
  if (_consecutive_failures != FAILURES_TO_LOST) return;

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
  _jump_pending = false;
  _last_probe_ms = agora;
  event_log(SEV_ERROR, COMP_TEMP, "temp.sensor_lost",
            "DS18B20 parou de responder apos %u leituras invalidas seguidas",
            (unsigned)_consecutive_failures);
}

static void _report_spike(float suspeito, float agua) {
  event_log(SEV_WARN, COMP_TEMP, "temp.spike_discarded",
            "Salto para %.2fC nao confirmado na releitura (agua em %.2fC); descartado",
            suspeito, agua);
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
    // precisa saber a temperatura antes de decidir qualquer coisa. A
    // releitura de um salto também não (`_last_conversion_ms = 0` abaixo).
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

  // CRC e "scratchpad todo zero" conferidos pela biblioteca. O endereço vem
  // da sondagem: `getTempCByIndex()` refazia a busca no 1-Wire a cada
  // conversão — mais tráfego no fio, mais chance de corromper.
  uint8_t sp[9];
  uint32_t t0 = millis();
  bool lido = _sensors.isConnected(_addr, sp);
  dbg_mark(DBG_OW_READ, 0, (uint16_t)(millis() - t0));
  if (!lido) {
    _count_failure(agora);
    return;
  }
  // DS18B20 a 12 bits: registrador de 16 bits com sinal, 1/16 °C por passo.
  float t = (int16_t)((sp[1] << 8) | sp[0]) / 16.0f;
  if (dbg_temp_fake_85 > 0) {
    dbg_temp_fake_85 = dbg_temp_fake_85 - 1;
    t = POWER_ON_RESET_C;
  }

  // Conversão exata, sem margem: 0x0550 vira exatamente 85.0f.
  if (t == POWER_ON_RESET_C) {
    event_log(SEV_WARN, COMP_TEMP, "temp.reset_value",
              "DS18B20 devolveu 85.0C, o valor de power-on; leitura descartada");
    _count_failure(agora);
    return;
  }

  if (t < TEMP_PLAUSIBLE_MIN_C || t > TEMP_PLAUSIBLE_MAX_C) {
    // O scratchpad cru diz se foi CRC fraco ou ruído no fio — sem ele, o
    // -48,00 de produção não tinha como ser investigado.
    event_log(SEV_WARN, COMP_TEMP, "temp.implausible",
              "Leitura impossivel descartada: %.2fC [%02X %02X %02X %02X %02X %02X %02X %02X %02X]",
              t, sp[0], sp[1], sp[2], sp[3], sp[4], sp[5], sp[6], sp[7], sp[8]);
    _count_failure(agora);
    return;
  }

  bool salto = _has_valid_reading && fabsf(t - _cached_celsius) > JUMP_CONFIRM_C;
  if (salto) {
    bool confirma = _jump_pending && fabsf(t - _jump_value) <= CONFIRM_TOLERANCE_C;
    if (!confirma) {
      if (_jump_pending) _report_spike(_jump_value, _cached_celsius);
      // Segura o valor anterior e relê já, sem esperar os 5 s: uma mudança
      // real se confirma em ~1 s.
      _jump_pending = true;
      _jump_value = t;
      _last_conversion_ms = 0;
      return;
    }
  } else if (_jump_pending) {
    // A releitura voltou para perto da água: o salto era leitura ruim.
    _report_spike(_jump_value, t);
  }
  _jump_pending = false;

  _cached_celsius = t;
  _has_valid_reading = true;
  _last_valid_ms = agora;
  _consecutive_failures = 0;
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
