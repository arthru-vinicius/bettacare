#pragma once
#include <stdint.h>
#include <string.h>

#include "OneWire.h"

typedef uint8_t DeviceAddress[8];

/** O "sensor" do outro lado do fio — o teste decide o que ele devolve. */
struct FakeDs18b20 {
  int count = 1;        // dispositivos no barramento
  bool crc_ok = true;   // falso: isConnected() falha, como no CRC ruim
  uint8_t sp[9] = {0x00, 0x00, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0x00};
  int conversions = 0;

  /** Grava a temperatura no registrador, como o DS18B20 a 12 bits (1/16 °C). */
  void set_celsius(float c) {
    int16_t raw = (int16_t)(c * 16.0f + (c >= 0 ? 0.5f : -0.5f));
    sp[0] = (uint8_t)(raw & 0xFF);
    sp[1] = (uint8_t)((raw >> 8) & 0xFF);
  }
};
inline FakeDs18b20 g_ds;

class DallasTemperature {
 public:
  explicit DallasTemperature(OneWire*) {}
  void begin() {}
  int getDeviceCount() { return g_ds.count; }
  bool getAddress(uint8_t* a, uint8_t i) {
    if (i >= g_ds.count) return false;
    const uint8_t rom[8] = {0x28, 1, 2, 3, 4, 5, 6, 7};
    memcpy(a, rom, 8);
    return true;
  }
  void setResolution(uint8_t) {}
  void setWaitForConversion(bool) {}
  void requestTemperatures() { g_ds.conversions++; }
  /** Como a biblioteca: lê o scratchpad e confere CRC e "tudo zero". */
  bool isConnected(const uint8_t*, uint8_t* sp) {
    if (g_ds.count == 0 || !g_ds.crc_ok) return false;
    memcpy(sp, g_ds.sp, 9);
    return true;
  }
};
