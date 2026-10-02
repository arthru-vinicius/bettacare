// RTClib falso: begin/lostPower/adjust sobre o FakeWire.
#pragma once
#include <stdint.h>

#include "Wire.h"

struct DateTime {
  uint8_t hh, mm;
  DateTime(uint16_t, uint8_t, uint8_t, uint8_t h, uint8_t m, uint8_t) : hh(h), mm(m) {}
};

inline uint8_t _bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

extern bool g_rtc_lost_power;
extern int g_adjust_calls;

class RTC_DS3231 {
 public:
  bool begin() { return Wire.end_error == 0; }
  bool lostPower() { return g_rtc_lost_power; }
  void adjust(const DateTime& dt) {
    g_adjust_calls++;
    Wire.reg_min = _bin2bcd(dt.mm);
    Wire.reg_hour = _bin2bcd(dt.hh);
    g_rtc_lost_power = false;
  }
};
