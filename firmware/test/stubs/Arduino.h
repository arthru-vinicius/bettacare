// Stub mínimo do core Arduino-ESP32 para testar a lógica do firmware no PC.
#pragma once
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>

#define IRAM_ATTR
#define HIGH 1
#define LOW 0
#define INPUT 0x01
#define OUTPUT 0x03
#define INPUT_PULLUP 0x05
#define FALLING 0x02

extern uint32_t g_millis;
inline uint32_t millis() { return g_millis; }
inline void delay(uint32_t ms) { g_millis += ms; }

// GPIO, ADC e LEDC: o teste lê o que o firmware escreveu e decide o que ele lê.
inline int g_pin[64] = {};    // nível de cada pino (digitalWrite/digitalRead)
inline int g_ledc[64] = {};   // duty escrito em cada pino de PWM
inline int g_adc = 0;         // o que analogRead() devolve
inline void pinMode(int, int) {}
inline void digitalWrite(int pin, int v) { g_pin[pin] = v; }
inline int digitalRead(int pin) { return g_pin[pin]; }
inline int analogRead(int) { return g_adc; }
inline bool ledcAttach(int, int, int) { return true; }
inline void ledcWrite(int pin, int duty) { g_ledc[pin] = duty; }
inline int digitalPinToInterrupt(int pin) { return pin; }
inline void attachInterrupt(int, void (*)(), int) {}
inline void noInterrupts() {}
inline void interrupts() {}
#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}
using std::max;

class String {
  std::string s;

 public:
  String() {}
  String(const char* c) : s(c ? c : "") {}
  const char* c_str() const { return s.c_str(); }
  size_t length() const { return s.size(); }
  bool operator==(const char* c) const { return s == c; }
};

typedef void* SemaphoreHandle_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return (void*)1; }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, uint32_t) { return pdTRUE; }
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }

struct FakeSerial {
  std::string in;
  size_t pos = 0;
  std::string out;
  void begin(...) {}
  int available() { return (int)(in.size() - pos); }
  int read() { return pos < in.size() ? (unsigned char)in[pos++] : -1; }
  size_t print(const char* c) {
    out += c;
    return strlen(c);
  }
  size_t printf(const char* fmt, ...) {
    char b[256];
    va_list a;
    va_start(a, fmt);
    int n = vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    out += b;
    return (size_t)n;
  }
};
extern FakeSerial Serial, Serial2;
#define SERIAL_8N1 0

struct tm;
inline void configTime(long, int, const char*, const char*) {}
bool getLocalTime(struct tm* info, uint32_t ms);
