#include "timeutil.h"

static int32_t _days_from_civil(int32_t y, uint32_t m, uint32_t d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = (uint32_t)(y - era * 400);
  const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

uint32_t civil_to_epoch(const CivilTime& t) {
  int32_t dias = _days_from_civil(t.year, t.month, t.day);
  return (uint32_t)dias * 86400UL + (uint32_t)t.hour * 3600UL + (uint32_t)t.minute * 60UL + t.second;
}

CivilTime epoch_to_civil(uint32_t epoch) {
  int32_t z = (int32_t)(epoch / 86400UL) + 719468;
  uint32_t resto = epoch % 86400UL;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = (uint32_t)(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  const uint32_t d = doy - (153 * mp + 2) / 5 + 1;
  const uint32_t m = mp < 10 ? mp + 3 : mp - 9;
  int32_t y = (int32_t)yoe + era * 400 + (m <= 2);

  CivilTime t;
  t.year = (uint16_t)y;
  t.month = (uint8_t)m;
  t.day = (uint8_t)d;
  t.hour = (uint8_t)(resto / 3600);
  t.minute = (uint8_t)((resto % 3600) / 60);
  t.second = (uint8_t)(resto % 60);
  return t;
}

bool civil_valid(const CivilTime& t) {
  static const uint8_t DIAS[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (t.year < 2024 || t.year > 2099) return false;
  if (t.month < 1 || t.month > 12) return false;
  if (t.day < 1 || t.day > DIAS[t.month - 1]) return false;
  bool bissexto = (t.year % 4 == 0 && t.year % 100 != 0) || t.year % 400 == 0;
  if (t.month == 2 && t.day == 29 && !bissexto) return false;
  return t.hour < 24 && t.minute < 60 && t.second < 60;
}
