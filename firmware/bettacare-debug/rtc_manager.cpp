#include "rtc_manager.h"

#include <RTClib.h>
#include <Wire.h>
#include <esp_task_wdt.h>
#include <time.h>

#include "config.h"
#include "debug_probe.h"
#include "device_config.h"
#include "event_log.h"
#include "fan.h"
#include "light.h"
#include "wifi_manager.h"

static RTC_DS3231 _rtc;
static bool       _available   = false;
static bool       _lost_power  = false;

static const uint8_t DS3231_ADDR = 0x68;

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

/**
 * Leituras de hora aceitas desde o boot. A automação usa este contador para
 * exigir que uma virada de período seja confirmada por uma leitura posterior
 * — ver `rtc_check_automation()`.
 */
static volatile uint32_t _clock_reads = 0;

/**
 * Leituras seguidas que falharam (I²C sem resposta ou bytes inválidos). Com
 * `READ_FAILURES_TO_MISSING` seguidas, o módulo passa a contar como ausente —
 * antes, um DS3231 que parasse de responder em operação continuava
 * "disponível" para sempre, com a hora congelada no último valor bom e a
 * automação da luz parada sem nenhum aviso.
 */
static uint8_t       _read_failures = 0;
static const uint8_t READ_FAILURES_TO_MISSING = 3;

/**
 * Avisos acumulados pelas leituras, emitidos fora do mutex — ver `_probe()`
 * sobre a ordem dos mutex.
 */
static volatile bool _bad_read_pending = false;
static uint8_t       _bad_raw_hour = 0;
static uint8_t       _bad_raw_minute = 0;
static volatile bool _went_missing_pending = false;

enum ClockRead : uint8_t { CLOCK_OK, CLOCK_I2C_ERROR, CLOCK_INVALID };

/** BCD válido: unidade até 9, dezena até `max_dezena`. */
static bool _bcd_valid(uint8_t v, uint8_t max_dezena) {
  return (v & 0x0F) <= 9 && (v >> 4) <= max_dezena;
}

/**
 * Lê hora e minuto direto dos registradores do DS3231. Só com `_i2c_mutex`.
 *
 * Não usa `RTC_DS3231::now()` de propósito (UPGRADE/07): ele ignora o retorno
 * da transação I²C e, quando ela falha, decodifica como hora o que havia na
 * pilha — foi assim que "40:08" chegou ao cache e carimbou os eventos. Pior
 * que o valor impossível é o lixo com cara de hora válida, que nenhuma
 * checagem de faixa pega e que acenderia a luz fora do horário. Aqui a
 * transação é conferida, e cada byte precisa ser BCD em modo 24 h — o único
 * que `adjust()` grava. Qualquer outra coisa é leitura ruim, nunca hora.
 */
static ClockRead _read_hhmm_locked(uint8_t& h, uint8_t& m, uint8_t& raw_h, uint8_t& raw_m) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write((uint8_t)0x01);   // registrador dos minutos; o das horas vem logo depois
  if (Wire.endTransmission() != 0) return CLOCK_I2C_ERROR;
  if (Wire.requestFrom(DS3231_ADDR, (uint8_t)2) != 2) return CLOCK_I2C_ERROR;
  raw_m = (uint8_t)Wire.read();
  raw_h = (uint8_t)Wire.read();

  // Minutos: bit 7 sempre zero. Horas: bits 7 e 6 zero (o 6 ligado é modo 12 h).
  if ((raw_m & 0x80) || !_bcd_valid(raw_m, 5)) return CLOCK_INVALID;
  if ((raw_h & 0xC0) || !_bcd_valid(raw_h, 2)) return CLOCK_INVALID;
  m = (uint8_t)((raw_m >> 4) * 10 + (raw_m & 0x0F));
  h = (uint8_t)((raw_h >> 4) * 10 + (raw_h & 0x0F));
  return h <= 23 ? CLOCK_OK : CLOCK_INVALID;
}

/**
 * Lê o DS3231 e atualiza o cache. Só com `_i2c_mutex`.
 *
 * Devolve se a leitura valeu. Na falha o cache fica com o último valor bom, e
 * a contagem de falhas seguidas decide quando o módulo deixa de valer.
 */
static bool _refresh_cached_time_locked() {
  uint8_t h = 0, m = 0, raw_h = 0, raw_m = 0;
  ClockRead r = _read_hhmm_locked(h, m, raw_h, raw_m);

  // Ganchos do firmware de debug: simulam falha de I²C, bytes inválidos e
  // leitura válida porém errada, para exercitar os caminhos abaixo ao vivo.
  if (dbg_rtc_fail_reads > 0) {
    dbg_rtc_fail_reads = dbg_rtc_fail_reads - 1;
    r = CLOCK_I2C_ERROR;
  } else if (dbg_rtc_bad_reads > 0) {
    dbg_rtc_bad_reads = dbg_rtc_bad_reads - 1;
    raw_h = 0x40;
    raw_m = 0x08;
    r = CLOCK_INVALID;
  } else if (r == CLOCK_OK && dbg_rtc_offset_reads > 0) {
    dbg_rtc_offset_reads = dbg_rtc_offset_reads - 1;
    int32_t deslocado = ((int32_t)(h * 60 + m) + dbg_rtc_offset_min) % 1440;
    uint16_t falso = (uint16_t)(deslocado < 0 ? deslocado + 1440 : deslocado);
    h = (uint8_t)(falso / 60);
    m = (uint8_t)(falso % 60);
  }
  dbg_mark(DBG_I2C_READ, (uint16_t)r, (uint16_t)(h * 60 + m));

  if (r == CLOCK_OK) {
    _cached_minutes = (uint16_t)(h * 60 + m);
    _clock_reads = _clock_reads + 1;
    _read_failures = 0;
    return true;
  }

  if (r == CLOCK_INVALID) {
    _bad_raw_hour = raw_h;
    _bad_raw_minute = raw_m;
    _bad_read_pending = true;
  }
  if (_read_failures < 255) _read_failures++;
  if (_available && _read_failures >= READ_FAILURES_TO_MISSING) {
    // Três leituras seguidas sem hora é módulo fora, não ruído. Marcar como
    // ausente devolve o caso à sondagem periódica (`_probe()`), e enquanto
    // isso a automação segue pela hora interna do ESP32.
    _available = false;
    _went_missing_pending = true;
  }
  return false;
}

/**
 * Hora do relógio interno do ESP32, em minutos desde a meia-noite.
 *
 * Só vale depois de um NTP bem-sucedido: antes disso o relógio do sistema
 * marca 1970, e devolver isso como hora seria pior que não ter hora nenhuma.
 * Depois, o SNTP do ESP-IDF o ressincroniza sozinho a cada 3 h.
 */
static uint16_t _system_minutes() {
  time_t agora = time(nullptr);
  if (agora < 1704067200) return TIME_UNKNOWN;   // antes de 2024: nunca sincronizou
  struct tm info;
  localtime_r(&agora, &info);
  return (uint16_t)(info.tm_hour * 60 + info.tm_min);
}

/** Emite os avisos acumulados pelas leituras. Chamar SEM o mutex do I²C. */
static void _report_pending() {
  if (_bad_read_pending) {
    _bad_read_pending = false;
    event_log(SEV_WARN, COMP_RTC, "rtc.bad_read",
              "DS3231 devolveu hora invalida (registradores 0x%02X:0x%02X); leitura descartada",
              (unsigned)_bad_raw_hour, (unsigned)_bad_raw_minute);
  }
  if (_went_missing_pending) {
    _went_missing_pending = false;
    event_log(SEV_ERROR, COMP_RTC, "rtc.missing",
              "DS3231 parou de responder (%u leituras seguidas falharam); %s",
              (unsigned)READ_FAILURES_TO_MISSING,
              _system_minutes() != TIME_UNKNOWN ? "automacao segue pela hora do ESP32"
                                                : "automacao parada ate haver NTP");
  }
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

/**
 * Virada de período vista numa leitura e ainda não confirmada por outra —
 * ver `rtc_check_automation()`.
 */
static bool     _transition_pending = false;
static bool     _transition_to      = false;
static uint32_t _transition_read    = 0;

static unsigned long _last_probe_ms = 0;
static const unsigned long PROBE_INTERVAL_MS = 15000;

/**
 * Intervalo entre leituras do relógio pelo I²C (longevidade).
 *
 * Antes, `rtc_check_automation()` fazia uma transação I²C **a cada volta do
 * loop** — 5×/s, ou **432 mil transações por dia**. Para ler o quê? Os
 * minutos: uma grandeza que muda 1×/min. Era amostrar 300× mais rápido que o
 * fenômeno.
 *
 * O custo não é só CPU. Cada transação chaveia o barramento, e o SDA/SCL do
 * DS3231 corre no mesmo chicote dos outros sinais: menos tráfego é menos
 * acoplamento de ruído, e é ruído que quebra leitura de sensor.
 *
 * A 10 s, a automação da luminária reage a uma virada de período em no
 * máximo 20 s (uma leitura para ver, outra para confirmar) — imperceptível
 * para um horário de aquário, que é definido em minutos. E a hora que a
 * interface exibe vem do cache (`_cached_minutes`), atualizado nessa cadência.
 */
static const unsigned long CLOCK_READ_INTERVAL_MS = 10000;
static unsigned long _last_clock_read_ms = 0;

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
  bool perdeu_energia = false;
  if (encontrado) {
    perdeu_energia = _rtc.lostPower();
    // Responder no endereço não basta: "encontrado" é "dá para ler a hora".
    // Um módulo que só devolve lixo continua ausente para a automação.
    encontrado = _refresh_cached_time_locked();
  }
  bool mudou = (encontrado != _available);
  _available = encontrado;
  if (encontrado) _lost_power = perdeu_energia;
  _i2c_give();
  _report_pending();

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

bool rtc_probe_present() {
  // Pelo mesmo mutex de todo o resto: o autodiagnóstico roda no núcleo de
  // controle e o NTP pode estar escrevendo no DS3231 pelo núcleo de rede.
  if (!_i2c_take(200)) return false;
  Wire.beginTransmission(DS3231_ADDR);
  bool respondeu = (Wire.endTransmission() == 0);
  _i2c_give();
  return respondeu;
}

String rtc_get_time_str() {
  uint16_t m = _cached_minutes;   // leitura atômica; não toca o barramento
  if (m == TIME_UNKNOWN) return "--:--";
  char buf[8];   // o compilador não sabe que m < 1440; 8 cabe qualquer uint16_t
  snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(m / 60), (unsigned)(m % 60));
  return String(buf);
}

bool rtc_sync_ntp() {
  if (!wifi_is_connected()) return false;

  configTime(NTP_UTC_OFFSET, 0, NTP_SERVER1, NTP_SERVER2);

  // `getLocalTime()` espera até 5 s por padrão — e o watchdog da task de rede
  // estava, na prática, em 5 s com reinício (ver `net_task.cpp`). Sem internet
  // isso dava loop de reboot. Espera curta por tentativa, e o watchdog é
  // alimentado entre elas: o pior caso fica em ~12 s, sempre vigiado.
  struct tm info;
  bool ok = false;
  for (int i = 0; i < 8; i++) {
    if (getLocalTime(&info, 1000)) { ok = true; break; }
    esp_task_wdt_reset();
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
    _report_pending();
    event_log(SEV_INFO, COMP_RTC, "rtc.ntp_synced",
              "Hora sincronizada: %s", rtc_get_time_str().c_str());
  } else {
    event_log(SEV_INFO, COMP_RTC, "rtc.ntp_synced",
              "NTP ok, mas sem modulo RTC; usando a hora interna do ESP32");
  }
  return true;
}

void rtc_check_automation() {
  if (!dbg_rtc_enabled) return;

  unsigned long agora = millis();

  if (!_available && agora - _last_probe_ms >= PROBE_INTERVAL_MS) _probe();

  // A leitura é espaçada (ver `CLOCK_READ_INTERVAL_MS`); entre uma e outra, a
  // avaliação abaixo usa o cache — que é exatamente o que ela precisa, já que
  // a decisão é por minuto.
  if (_last_clock_read_ms == 0 || agora - _last_clock_read_ms >= CLOCK_READ_INTERVAL_MS) {
    if (_available) {
      // Espera curta para nunca segurar o loop se a task de rede estiver no
      // meio de um NTP; sem o mutex agora, tenta de novo na próxima volta.
      if (_i2c_take(20)) {
        _last_clock_read_ms = agora;
        _refresh_cached_time_locked();
        _i2c_give();
        _report_pending();
      }
    } else {
      // Sem o DS3231, a hora interna do ESP32. Antes de qualquer NTP ela não
      // vale nada, e a automação simplesmente espera.
      _last_clock_read_ms = agora;
      uint16_t sistema = _system_minutes();
      _cached_minutes = sistema;
      if (sistema != TIME_UNKNOWN) _clock_reads = _clock_reads + 1;
    }
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
  if (_period_known && deveria == _last_period_on) {
    _transition_pending = false;
    return;
  }

  // Fora do boot e da reaplicação, uma virada só vale quando uma SEGUNDA
  // leitura do relógio, posterior, concorda com ela. A validação acima barra
  // byte impossível, mas não um byte corrompido que ainda forma uma hora
  // válida — e uma leitura dessas, sozinha, acenderia a luz por 10 s e, na
  // volta, apagaria o override manual que o usuário tinha feito.
  if (_period_known) {
    if (!_transition_pending || _transition_to != deveria) {
      _transition_pending = true;
      _transition_to      = deveria;
      _transition_read    = _clock_reads;
      return;
    }
    if (_clock_reads == _transition_read) return;   // ainda a mesma leitura
  }

  _transition_pending = false;
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

void rtc_reapply_schedule() {
  // Força a reavaliação esquecendo o período conhecido: a próxima chamada de
  // `rtc_check_automation()` decide do zero com os horários novos.
  _period_known = false;
}
