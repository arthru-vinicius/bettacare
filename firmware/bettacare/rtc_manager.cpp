#include "rtc_manager.h"
#include "config.h"
#include "light.h"
#include "fan.h"
#include "wifi_manager.h"
#include "log_manager.h"
#include <RTClib.h>
#include <Wire.h>
#include <time.h>   // getLocalTime(), configTime() — ESP32 Arduino core
#include <Preferences.h>

static RTC_DS3231 _rtc;
static bool       _available      = false;

// Horários configurados em config.h
static uint8_t _on_hour    = RTC_ON_HOUR;
static uint8_t _on_minute  = RTC_ON_MIN;
static uint8_t _off_hour   = RTC_OFF_HOUR;
static uint8_t _off_minute = RTC_OFF_MIN;

// Rastreia o último período (ligado/desligado) para só agir na transição.
// Isso garante que sobrescritas manuais dentro de um período sejam respeitadas
// até a próxima mudança de janela horária.
static bool _last_period_on = false;

#define RTC_CFG_NS       "aq_cfg"
#define RTC_CFG_KEY_ON   "lt_on"
#define RTC_CFG_KEY_OFF  "lt_off"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static String _format_time(int h, int m) {
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
  return String(buf);
}

static bool _is_valid_time(uint8_t h, uint8_t m) {
  return h <= 23 && m <= 59;
}

static int _minutes_from_hm(uint8_t h, uint8_t m) {
  return ((int)h * 60) + (int)m;
}

static bool _is_valid_minutes(int mins) {
  return mins >= 0 && mins <= 1439;
}

static void _hm_from_minutes(int mins, uint8_t& h, uint8_t& m) {
  if (!_is_valid_minutes(mins)) {
    h = 0;
    m = 0;
    return;
  }
  h = (uint8_t)(mins / 60);
  m = (uint8_t)(mins % 60);
}

static void _load_schedule_nvs() {
  Preferences prefs;
  if (!prefs.begin(RTC_CFG_NS, true)) {
    Serial.println("[RTC] Falha ao abrir NVS. Usando horarios de config.h");
    return;
  }

  int defaultOn  = _minutes_from_hm((uint8_t)RTC_ON_HOUR, (uint8_t)RTC_ON_MIN);
  int defaultOff = _minutes_from_hm((uint8_t)RTC_OFF_HOUR, (uint8_t)RTC_OFF_MIN);
  int onMins     = prefs.getInt(RTC_CFG_KEY_ON, defaultOn);
  int offMins    = prefs.getInt(RTC_CFG_KEY_OFF, defaultOff);
  prefs.end();

  if (!_is_valid_minutes(onMins) || !_is_valid_minutes(offMins)) {
    _on_hour    = (uint8_t)RTC_ON_HOUR;
    _on_minute  = (uint8_t)RTC_ON_MIN;
    _off_hour   = (uint8_t)RTC_OFF_HOUR;
    _off_minute = (uint8_t)RTC_OFF_MIN;
    Serial.println("[RTC] Horarios em NVS invalidos. Usando config.h");
    return;
  }

  _hm_from_minutes(onMins, _on_hour, _on_minute);
  _hm_from_minutes(offMins, _off_hour, _off_minute);
  Serial.printf("[RTC] Horarios carregados: liga=%s desliga=%s\n",
                rtc_get_on_time().c_str(), rtc_get_off_time().c_str());
}

static void _save_schedule_nvs() {
  Preferences prefs;
  if (!prefs.begin(RTC_CFG_NS, false)) {
    Serial.println("[RTC] Falha ao abrir NVS para salvar horarios");
    return;
  }
  prefs.putInt(RTC_CFG_KEY_ON, _minutes_from_hm(_on_hour, _on_minute));
  prefs.putInt(RTC_CFG_KEY_OFF, _minutes_from_hm(_off_hour, _off_minute));
  prefs.end();
}

static bool _period_should_be_on(int current, int on_at, int off_at) {
  if (on_at < off_at) {
    // Caso normal: ex. 10:00 – 17:00
    return (current >= on_at && current < off_at);
  }
  // Cruzando meia-noite: ex. 20:00 – 06:00
  return (current >= on_at || current < off_at);
}

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------

void rtc_init() {
  _load_schedule_nvs();
  Wire.begin();
  _available = _rtc.begin();

  if (_available) {
    if (_rtc.lostPower()) {
      // Bateria fraca: hora pode estar errada — será corrigida pelo NTP em seguida
      Serial.println("[RTC] Bateria fraca ou primeiro uso. Hora sera ajustada via NTP.");
    }
    Serial.print("[RTC] DS3231SN encontrado. Hora atual: ");
    Serial.println(rtc_get_time_str());
  } else {
    Serial.println("[RTC] DS3231SN NAO encontrado. Verifique conexao I2C (SDA=21, SCL=22).");
  }
}

bool rtc_sync_ntp() {
  if (!wifi_is_connected()) {
    Serial.println("[RTC] WiFi indisponivel. Pulando sincronizacao NTP sem bloquear o sistema.");
    return false;
  }

  // Configura SNTP com fuso horário de Brasília (UTC-3, sem horário de verão)
  configTime(NTP_UTC_OFFSET, 0, NTP_SERVER1, NTP_SERVER2);

  Serial.print("[RTC] Sincronizando com NTP (Brasilia UTC-3)");

  struct tm timeinfo;
  bool synced = false;
  for (int i = 0; i < 8; i++) {
    if (getLocalTime(&timeinfo)) {
      synced = true;
      break;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (!synced) {
    Serial.println("[RTC] Timeout NTP. Usando hora ja armazenada no modulo.");
    return false;
  }

  // Atualiza o DS3231SN com a hora obtida via NTP
  if (_available) {
    DateTime dt(
      timeinfo.tm_year + 1900,
      timeinfo.tm_mon  + 1,
      timeinfo.tm_mday,
      timeinfo.tm_hour,
      timeinfo.tm_min,
      timeinfo.tm_sec
    );
    _rtc.adjust(dt);
    Serial.print("[RTC] Hora ajustada via NTP: ");
    Serial.println(rtc_get_time_str());
  } else {
    Serial.println("[RTC] NTP OK mas modulo RTC indisponivel — usando hora do sistema ESP32.");
  }

  return true;
}

String rtc_get_time_str() {
  if (!_available) return "--:--";
  DateTime now = _rtc.now();
  return _format_time(now.hour(), now.minute());
}

bool rtc_available() {
  return _available;
}

void rtc_check_automation() {
  if (!_available) return;

  DateTime now = _rtc.now();

  // Converte os horários para minutos desde meia-noite para facilitar a comparação
  int current = now.hour()   * 60 + now.minute();
  int on_at   = _on_hour     * 60 + _on_minute;
  int off_at  = _off_hour    * 60 + _off_minute;

  bool should_be_on = _period_should_be_on(current, on_at, off_at);

  // Só age quando o período muda (on → off ou off → on).
  // Dentro do mesmo período, sobrescritas manuais são preservadas.
  if (should_be_on != _last_period_on) {
    _last_period_on = should_be_on;
    light_set(should_be_on);
    if (should_be_on) {
      // Na transição para o período ON: reseta a ventoinha para modo AUTO.
      // Overrides manuais anteriores são descartados; pot requer re-calibração.
      fan_on_rtc_reset();
    }
    Serial.printf("[RTC] Automacao: %s luminaria\n", should_be_on ? "ligando" : "desligando");
  }
}

String rtc_get_on_time() {
  return _format_time(_on_hour, _on_minute);
}

String rtc_get_off_time() {
  return _format_time(_off_hour, _off_minute);
}

bool rtc_set_schedule(uint8_t on_hour, uint8_t on_min, uint8_t off_hour, uint8_t off_min) {
  if (!_is_valid_time(on_hour, on_min) || !_is_valid_time(off_hour, off_min)) {
    return false;
  }

  _on_hour    = on_hour;
  _on_minute  = on_min;
  _off_hour   = off_hour;
  _off_minute = off_min;
  _save_schedule_nvs();

  log_eventf("[RTC] Programacao atualizada: liga=%s desliga=%s",
             rtc_get_on_time().c_str(),
             rtc_get_off_time().c_str());
  Serial.printf("[RTC] Programacao atualizada: liga=%s desliga=%s\n",
                rtc_get_on_time().c_str(),
                rtc_get_off_time().c_str());

  // Reaplica imediatamente a regra atual para refletir a nova programação.
  if (_available) {
    DateTime now = _rtc.now();
    int current  = now.hour() * 60 + now.minute();
    int on_at    = _on_hour * 60 + _on_minute;
    int off_at   = _off_hour * 60 + _off_minute;
    bool should_be_on = _period_should_be_on(current, on_at, off_at);
    _last_period_on = !should_be_on;
    rtc_check_automation();
  }

  return true;
}
