#include "rtc_manager.h"

#include <RTClib.h>
#include <Wire.h>
#include <time.h>

#include "config.h"
#include "device_config.h"
#include "event_log.h"
#include "fan.h"
#include "light.h"
#include "wifi_manager.h"

static RTC_DS3231 _rtc;
static bool       _available   = false;
static bool       _lost_power  = false;

/**
 * O barramento I2C é tocado pelos **dois núcleos**: o controle local avalia a
 * automação, e a task de rede sincroniza por NTP. O I2C não é reentrante —
 * duas transações simultâneas travam o barramento ou devolvem lixo. Toda
 * conversa com o DS3231 passa por este mutex.
 */
static SemaphoreHandle_t _i2c_mutex = nullptr;

/**
 * Hora corrente em minutos desde a meia-noite, ou `TIME_UNKNOWN`.
 *
 * `rtc_get_time_str()` é chamado de dentro de `event_log()`, ou seja, de
 * qualquer núcleo e a qualquer momento. Se ele fizesse I2C, cada linha de log
 * viraria uma disputa pelo barramento — e, pior, um ciclo de travamento entre
 * o mutex do log e o do I2C. Lendo deste cache, a função não toca hardware
 * nenhum.
 *
 * `uint16_t` alinhado é lido e escrito em uma instrução no ESP32, então não há
 * leitura rasgada nem necessidade de mutex aqui.
 */
static const uint16_t TIME_UNKNOWN = 0xFFFF;
static volatile uint16_t _cached_minutes = TIME_UNKNOWN;

/** Atualiza o cache. Só pode ser chamada já segurando `_i2c_mutex`. */
static void _refresh_cached_time_locked() {
  if (!_available) {
    _cached_minutes = TIME_UNKNOWN;
    return;
  }
  DateTime agora = _rtc.now();
  _cached_minutes = (uint16_t)(agora.hour() * 60 + agora.minute());
}

static bool _i2c_take(uint32_t ms) {
  if (_i2c_mutex == nullptr) return false;
  return xSemaphoreTake(_i2c_mutex, pdMS_TO_TICKS(ms)) == pdTRUE;
}

static void _i2c_give() {
  if (_i2c_mutex != nullptr) xSemaphoreGive(_i2c_mutex);
}

/**
 * Último período conhecido. É o que faz a automação agir só na transição — e
 * portanto o que faz um override manual sobreviver dentro da janela.
 */
static bool _last_period_on = false;
static bool _period_known   = false;

static unsigned long _last_probe_ms = 0;
static const unsigned long PROBE_INTERVAL_MS = 15000;

static bool _period_should_be_on(int current, int on_at, int off_at) {
  if (on_at < off_at) return (current >= on_at && current < off_at);
  // Janela cruzando meia-noite, ex.: 20:00–06:00
  return (current >= on_at || current < off_at);
}

/**
 * Procura o módulo e atualiza o cache.
 *
 * Registra os eventos **depois de soltar o mutex**, de propósito: `event_log()`
 * pega o próprio mutex, e logar segurando o do I2C criaria uma ordem de
 * travamento invertida em relação a `rtc_get_time_str()` — receita de deadlock
 * entre os dois núcleos.
 */
static void _probe() {
  _last_probe_ms = millis();

  if (!_i2c_take(200)) return;
  bool encontrado = _rtc.begin();
  bool mudou = (encontrado != _available);
  _available = encontrado;
  bool perdeu_energia = false;
  if (encontrado) {
    perdeu_energia = _rtc.lostPower();
    _lost_power = perdeu_energia;
  }
  _refresh_cached_time_locked();
  _i2c_give();

  if (!mudou) return;

  if (encontrado) {
    event_log(SEV_INFO, COMP_RTC, "rtc.found",
              "DS3231 reconhecido; hora %s", rtc_get_time_str().c_str());
    if (perdeu_energia) {
      event_log(SEV_WARN, COMP_RTC, "rtc.lost_power",
                "Relogio perdeu a hora; a bateria provavelmente acabou");
    }
  } else {
    event_log(SEV_ERROR, COMP_RTC, "rtc.missing",
              "DS3231 nao responde no I2C; verifique SDA (21) e SCL (22)");
  }
}

void rtc_init() {
  _i2c_mutex = xSemaphoreCreateMutex();
  Wire.begin();
  _available = false;
  _probe();
}

bool rtc_available()  { return _available; }
bool rtc_lost_power() { return _lost_power; }

String rtc_get_time_str() {
  uint16_t m = _cached_minutes;   // leitura atômica; não toca o barramento
  if (m == TIME_UNKNOWN) return "--:--";
  char buf[6];
  snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(m / 60), (unsigned)(m % 60));
  return String(buf);
}

bool rtc_sync_ntp() {
  if (!wifi_is_connected()) return false;

  configTime(NTP_UTC_OFFSET, 0, NTP_SERVER1, NTP_SERVER2);

  struct tm info;
  bool ok = false;
  for (int i = 0; i < 8; i++) {
    if (getLocalTime(&info)) { ok = true; break; }
    delay(500);   // aceitável: roda na task de rede, não no loop
  }

  if (!ok) {
    event_log(SEV_WARN, COMP_RTC, "rtc.ntp_timeout",
              "NTP nao respondeu; mantendo a hora do modulo");
    return false;
  }

  if (_available) {
    if (!_i2c_take(500)) return false;
    _rtc.adjust(DateTime(info.tm_year + 1900, info.tm_mon + 1, info.tm_mday,
                         info.tm_hour, info.tm_min, info.tm_sec));
    _lost_power = false;
    _refresh_cached_time_locked();
    _i2c_give();
    event_log(SEV_INFO, COMP_RTC, "rtc.ntp_synced",
              "Hora sincronizada: %s", rtc_get_time_str().c_str());
  } else {
    event_log(SEV_INFO, COMP_RTC, "rtc.ntp_synced",
              "NTP ok, mas sem modulo RTC; usando a hora interna do ESP32");
  }
  return true;
}

void rtc_check_automation() {
  if (!_available) {
    if (millis() - _last_probe_ms >= PROBE_INTERVAL_MS) _probe();
    return;
  }

  // Só o controle local lê o relógio no caminho quente; espera curta para
  // nunca segurar o loop se a task de rede estiver no meio de um NTP.
  if (_i2c_take(20)) {
    _refresh_cached_time_locked();
    _i2c_give();
  }

  uint16_t m = _cached_minutes;
  if (m == TIME_UNKNOWN) return;

  const DeviceConfig cfg = device_config_snapshot();

  int atual  = (int)m;
  int liga   = cfg.light_on_hour  * 60 + cfg.light_on_min;
  int apaga  = cfg.light_off_hour * 60 + cfg.light_off_min;

  bool deveria = _period_should_be_on(atual, liga, apaga);

  // Só age na mudança de período. Dentro do mesmo período, o que o usuário
  // decidiu manualmente permanece.
  if (!_period_known || deveria != _last_period_on) {
    _period_known   = true;
    _last_period_on = deveria;
    light_set(deveria, LIGHT_SRC_SCHEDULE);
    if (deveria) {
      // Na virada para o período ON, a ventoinha volta ao automático e o pot
      // precisa ser recalibrado.
      fan_on_schedule_reset();
    }
    event_log(SEV_INFO, COMP_RTC, "rtc.automation",
              "Automacao por horario: %s luminaria (%s-%s)",
              deveria ? "acendendo" : "apagando",
              device_config_on_time().c_str(),
              device_config_off_time().c_str());
  }
}

void rtc_reapply_schedule() {
  // Força a reavaliação esquecendo o período conhecido: a próxima chamada de
  // `rtc_check_automation()` decide do zero com os horários novos.
  _period_known = false;
}
