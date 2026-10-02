#include "clock.h"

#include <Arduino.h>
#include <Wire.h>
#include <time.h>

#include "config.h"
#include "debuglog.h"
#include "wifi_manager.h"

#ifndef NTP_SERVER_1
#define NTP_SERVER_1 "a.st1.ntp.br"
#endif
#ifndef NTP_SERVER_2
#define NTP_SERVER_2 "pool.ntp.org"
#endif
#ifndef CLOCK_UTC_OFFSET_S
#define CLOCK_UTC_OFFSET_S (-3L * 3600L)   // Brasília, sem horário de verão
#endif

static const uint8_t  DS3231 = 0x68;
static const uint32_t REREAD_MS = 60000;      // rebase pelo DS3231
static const uint32_t NTP_CHECK_MS = 600000;  // compara com o NTP a cada 10 min
static const uint8_t  FALHAS_PARA_AUSENTE = 3;

static bool     _valid = false;
static bool     _rtc_ok = false;
static uint32_t _base_epoch = 0;
static uint32_t _base_ms = 0;
static uint32_t _last_read_ms = 0;
static uint8_t  _falhas = 0;
static bool     _ntp_started = false;
static bool     _ntp_synced = false;
static uint32_t _ntp_check_ms = 0;

static uint8_t _bcd(uint8_t v, bool& ok) {
  if ((v & 0x0F) > 9 || (v >> 4) > 9) ok = false;
  return (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}

static uint8_t _to_bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static bool _read_reg(uint8_t reg, uint8_t* out, uint8_t n) {
  Wire.beginTransmission(DS3231);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)DS3231, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) out[i] = (uint8_t)Wire.read();
  return true;
}

/** O DS3231 parou por falta de energia (bit OSF): a hora dele não vale. */
static bool _lost_power(bool& ok) {
  uint8_t st = 0;
  ok = _read_reg(0x0F, &st, 1);
  return ok && (st & 0x80);
}

static bool _read_rtc(CivilTime& t) {
  uint8_t r[7];
  if (!_read_reg(0x00, r, 7)) return false;
  if (r[2] & 0x40) return false;   // modo 12 h: o firmware sempre grava 24 h
  bool ok = true;
  t.second = _bcd(r[0] & 0x7F, ok);
  t.minute = _bcd(r[1] & 0x7F, ok);
  t.hour   = _bcd(r[2] & 0x3F, ok);
  t.day    = _bcd(r[4] & 0x3F, ok);
  t.month  = _bcd(r[5] & 0x1F, ok);
  t.year   = (uint16_t)(2000 + _bcd(r[6], ok));
  return ok && civil_valid(t);
}

static bool _write_rtc(const CivilTime& t) {
  Wire.beginTransmission(DS3231);
  Wire.write((uint8_t)0x00);
  Wire.write(_to_bcd(t.second));
  Wire.write(_to_bcd(t.minute));
  Wire.write(_to_bcd(t.hour));   // bit 6 em zero: 24 h
  Wire.write((uint8_t)1);        // dia da semana: não usado
  Wire.write(_to_bcd(t.day));
  Wire.write(_to_bcd(t.month));
  Wire.write(_to_bcd((uint8_t)(t.year - 2000)));
  if (Wire.endTransmission() != 0) return false;
  // Limpa o OSF: a hora agora é confiável.
  uint8_t st = 0;
  if (!_read_reg(0x0F, &st, 1)) return false;
  Wire.beginTransmission(DS3231);
  Wire.write((uint8_t)0x0F);
  Wire.write((uint8_t)(st & 0x7F));
  return Wire.endTransmission() == 0;
}

static void _rebase(uint32_t epoch) {
  _base_epoch = epoch;
  _base_ms = millis();
}

void clock_init() {
  bool ok = false;
  bool perdeu = _lost_power(ok);
  _rtc_ok = ok;
  if (!ok) {
    Log.println("[Relogio] DS3231 nao responde no I2C; hora so pelo NTP");
    return;
  }
  CivilTime t;
  if (!perdeu && _read_rtc(t)) {
    _rebase(civil_to_epoch(t));
    _valid = true;
  } else {
    Log.println("[Relogio] DS3231 sem hora valida (bateria?); esperando NTP ou o comando 'hora'");
  }
  _last_read_ms = millis();
}

static void _reread() {
  if (!_rtc_ok) return;
  CivilTime t;
  bool ok = false;
  bool perdeu = _lost_power(ok);
  if (ok && !perdeu && _read_rtc(t)) {
    _falhas = 0;
    _rebase(civil_to_epoch(t));
    _valid = true;
    return;
  }
  // Leitura ruim: fica com a hora andando pelo millis(). Repetindo, o módulo
  // conta como ausente e a hora segue pelo NTP, se houver.
  if (++_falhas >= FALHAS_PARA_AUSENTE) {
    _rtc_ok = false;
    Log.println("[Relogio] DS3231 parou de responder");
  }
}

static void _ntp() {
  if (!wifi_is_connected()) return;
  if (!_ntp_started) {
    configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);   // UTC; o fuso é aplicado aqui
    _ntp_started = true;
    _ntp_check_ms = millis() - NTP_CHECK_MS + 5000;  // primeira conferência em ~5 s
  }
  if (millis() - _ntp_check_ms < NTP_CHECK_MS) return;
  _ntp_check_ms = millis();

  time_t utc = time(nullptr);
  if (utc < 1700000000) {
    _ntp_check_ms = millis() - NTP_CHECK_MS + 5000;  // ainda sem resposta: tenta de novo em 5 s
    return;
  }
  uint32_t local = (uint32_t)((int64_t)utc + CLOCK_UTC_OFFSET_S);
  _ntp_synced = true;
  int64_t diferenca = (int64_t)local - (int64_t)clock_now();
  if (_valid && diferenca >= -2 && diferenca <= 2) return;

  CivilTime t = epoch_to_civil(local);
  if (_rtc_ok && !_write_rtc(t)) Log.println("[Relogio] Falha ao acertar o DS3231 pelo NTP");
  _rebase(local);
  _valid = true;
  Log.printf("[Relogio] Acertado pelo NTP: %04u-%02u-%02u %02u:%02u\n", t.year, t.month, t.day,
                t.hour, t.minute);
}

void clock_update() {
  if (millis() - _last_read_ms >= REREAD_MS) {
    _last_read_ms = millis();
    _reread();
  }
  _ntp();
}

bool clock_valid() { return _valid; }

uint32_t clock_now() { return _base_epoch + (millis() - _base_ms) / 1000UL; }

bool clock_set(const CivilTime& t) {
  if (!civil_valid(t)) return false;
  if (_rtc_ok && !_write_rtc(t)) return false;
  _rebase(civil_to_epoch(t));
  _valid = true;
  return true;
}

bool clock_rtc_present() { return _rtc_ok; }
bool clock_ntp_synced() { return _ntp_synced; }
