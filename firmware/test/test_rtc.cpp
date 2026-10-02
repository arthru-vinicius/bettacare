// Teste de host da lógica de rtc_manager.cpp (o arquivo real, incluído inteiro).
#define _POSIX_THREAD_SAFE_FUNCTIONS 200112L
#include <time.h>

#include <string>
#include <vector>

static time_t g_fake_time = 0;
static time_t fake_time(time_t* t) {
  if (t) *t = g_fake_time;
  return g_fake_time;
}
#define time(x) fake_time(x)

#include "rtc_manager.cpp"   // o do firmware, via -I

// ── Definições que o firmware espera dos outros módulos ─────────────────────
uint32_t g_millis = 1000;
FakeSerial Serial, Serial2;
FakeWire Wire;
bool g_rtc_lost_power = false;
int g_adjust_calls = 0;
static bool g_wifi = true;


struct Ev { std::string code, msg; };
static std::vector<Ev> g_events;
void event_log(EventSeverity, EventComponent, const char* code, const char* fmt, ...) {
  char b[256];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof b, fmt, a);
  va_end(a);
  g_events.push_back({code, b});
}

static DeviceConfig g_cfg = {10, 0, 17, 0, 29.0f, 27.5f, 3000, 60000};
DeviceConfig device_config_snapshot() { return g_cfg; }
String device_config_on_time() { return "10:00"; }
String device_config_off_time() { return "17:00"; }

struct LightCall { bool on; LightSource src; };
static std::vector<LightCall> g_light;
void light_set(bool on, LightSource src) { g_light.push_back({on, src}); }
static int g_fan_resets = 0;
void fan_on_schedule_reset() { g_fan_resets++; }
bool wifi_is_connected() { return g_wifi; }

bool getLocalTime(struct tm* info, uint32_t) {
  if (g_fake_time < 1704067200) return false;
  time_t t = g_fake_time;
  localtime_r(&t, info);
  return true;
}

// ── Mini framework ───────────────────────────────────────────────────────────
static int g_fail = 0, g_ok = 0;
#define CHECK(cond, what)                                              \
  do {                                                                 \
    if (cond) { g_ok++; }                                              \
    else { g_fail++; printf("  FALHOU: %s  (linha %d)\n", what, __LINE__); } \
  } while (0)

static void set_clock(int h, int m) {
  Wire.reg_hour = _bin2bcd((uint8_t)h);
  Wire.reg_min = _bin2bcd((uint8_t)m);
}
static void tick(uint32_t ms) {
  g_millis += ms;
  rtc_check_automation();
}
static bool has_event(const char* code) {
  for (auto& e : g_events) if (e.code == code) return true;
  return false;
}
static const Ev* last_event(const char* code) {
  const Ev* r = nullptr;
  for (auto& e : g_events) if (e.code == code) r = &e;
  return r;
}

int main() {
  printf("T1 boot no periodo ON: age na hora, sem esperar confirmacao\n");
  set_clock(12, 30);
  rtc_init();
  CHECK(rtc_available(), "modulo encontrado no boot");
  CHECK(has_event("rtc.found"), "evento rtc.found");
  rtc_check_automation();
  CHECK(g_light.size() == 1 && g_light[0].on && g_light[0].src == LIGHT_SRC_SCHEDULE,
        "acendeu pela automacao no boot");
  CHECK(strcmp(rtc_get_time_str().c_str(), "12:30") == 0, "cache 12:30");

  printf("T2 uma leitura valida porem errada (cruza o horario) nao troca a luz\n");
  for (int i = 0; i < 20; i++) tick(200);   // 4 s: sem leitura nova
  set_clock(18, 30);                        // UMA leitura corrompida: 18:30, periodo OFF
  tick(6000);                               // 10 s desde a ultima leitura
  set_clock(12, 30);
  CHECK(strcmp(rtc_get_time_str().c_str(), "18:30") == 0, "cache recebeu a leitura errada");
  for (int i = 0; i < 40; i++) tick(200);   // 8 s de loop na mesma leitura
  CHECK(g_light.size() == 1, "nenhuma troca so com a leitura errada");
  tick(2000);                               // proxima leitura, de volta a 12:30
  CHECK(strcmp(rtc_get_time_str().c_str(), "12:30") == 0, "leitura seguinte correta");
  for (int i = 0; i < 60; i++) tick(200);
  CHECK(g_light.size() == 1, "luz continua acesa, override preservado");

  printf("T3 virada real: confirma na segunda leitura\n");
  set_clock(17, 0);
  tick(10000);
  CHECK(g_light.size() == 1, "primeira leitura de 17:00 so marca candidata");
  for (int i = 0; i < 40; i++) tick(200);
  CHECK(g_light.size() == 1, "mesma leitura nao confirma a si mesma");
  set_clock(17, 0);
  tick(2000);
  CHECK(g_light.size() == 2 && !g_light[1].on, "segunda leitura confirma: apagou");
  CHECK(has_event("rtc.automation"), "evento rtc.automation");
  for (int i = 0; i < 100; i++) tick(200);
  CHECK(g_light.size() == 2, "nada mais depois da virada");

  printf("T4 tres falhas de I2C seguidas: modulo ausente, sem NTP a automacao espera\n");
  g_events.clear();
  Wire.end_error = 2;
  tick(10000);
  tick(10000);
  CHECK(rtc_available(), "duas falhas ainda nao derrubam o modulo");
  CHECK(strcmp(rtc_get_time_str().c_str(), "17:00") == 0, "cache mantem o ultimo valor bom");
  tick(10000);
  CHECK(!rtc_available(), "terceira falha: modulo ausente");
  const Ev* miss = last_event("rtc.missing");
  CHECK(miss != nullptr, "evento rtc.missing");
  if (miss) printf("    msg: %s\n", miss->msg.c_str());
  CHECK(miss && strstr(miss->msg.c_str(), "parada ate haver NTP"), "mensagem diz que espera NTP");
  tick(10000);
  CHECK(strcmp(rtc_get_time_str().c_str(), "--:--") == 0, "sem NTP o cache vira desconhecido");
  size_t antes = g_light.size();
  for (int i = 0; i < 50; i++) tick(200);
  CHECK(g_light.size() == antes, "sem hora nenhuma, a automacao nao age");

  printf("T5 com NTP, a automacao segue pela hora do ESP32\n");
  struct tm alvo = {};
  alvo.tm_year = 2026 - 1900; alvo.tm_mon = 9; alvo.tm_mday = 1;
  alvo.tm_hour = 18; alvo.tm_min = 5; alvo.tm_isdst = -1;
  g_fake_time = mktime(&alvo);
  tick(10000);
  CHECK(strcmp(rtc_get_time_str().c_str(), "18:05") == 0, "cache = hora do sistema 18:05");
  CHECK(g_light.size() == antes, "18:05 e periodo OFF: nada a fazer");
  alvo.tm_mday = 2; alvo.tm_hour = 10; alvo.tm_min = 1;   // dia seguinte, 10:01: periodo ON
  g_fake_time = mktime(&alvo);
  tick(10000);
  tick(10000);
  CHECK(g_light.size() == antes + 1 && g_light.back().on, "virada pela hora do sistema, confirmada");

  printf("T6 modulo volta: a sondagem o reencontra\n");
  Wire.end_error = 0;
  set_clock(10, 2);
  g_events.clear();
  tick(15000);
  CHECK(rtc_available(), "reencontrado");
  CHECK(has_event("rtc.found"), "evento rtc.found");

  printf("T7 bytes invalidos: descarta, avisa e, repetindo, derruba\n");
  g_events.clear();
  Wire.reg_hour = 0x40;   // bit de modo 12 h: o "40:08" de antes
  Wire.reg_min = 0x08;
  tick(10000);
  const Ev* bad = last_event("rtc.bad_read");
  CHECK(bad != nullptr, "evento rtc.bad_read");
  if (bad) printf("    msg: %s\n", bad->msg.c_str());
  CHECK(strcmp(rtc_get_time_str().c_str(), "10:02") == 0, "cache intacto");
  Wire.reg_min = 0x5A;    // nibble 0xA nao e BCD
  Wire.reg_hour = 0x10;
  tick(10000);
  Wire.reg_min = 0x00;
  Wire.reg_hour = 0x24;   // 24 h nao existe
  tick(10000);
  CHECK(!rtc_available(), "tres leituras invalidas seguidas: ausente");
  CHECK(strstr(last_event("rtc.missing")->msg.c_str(), "segue pela hora do ESP32"),
        "com NTP, a mensagem diz que segue pela hora do ESP32");

  printf("T8 NTP: ajusta o DS3231 e o cache\n");
  set_clock(10, 2);
  tick(15000);   // reencontra
  CHECK(rtc_available(), "disponivel de novo");
  alvo.tm_hour = 10; alvo.tm_min = 33;
  g_fake_time = mktime(&alvo);
  g_events.clear();
  CHECK(rtc_sync_ntp(), "rtc_sync_ntp ok");
  CHECK(g_adjust_calls == 1, "adjust chamado");
  CHECK(strcmp(rtc_get_time_str().c_str(), "10:33") == 0, "cache = hora do NTP");
  CHECK(has_event("rtc.ntp_synced"), "evento rtc.ntp_synced");

  printf("T9 reaplicacao (config nova): age na hora, sem confirmacao\n");
  g_cfg.light_on_hour = 11;   // 10:33 passa a ser periodo OFF
  rtc_reapply_schedule();
  size_t n = g_light.size();
  rtc_check_automation();
  CHECK(g_light.size() == n + 1 && !g_light.back().on, "apagou imediatamente");

  printf("T10 config nova sem reaplicacao: troca so na leitura seguinte\n");
  g_cfg.light_on_hour = 10;   // volta a ser periodo ON
  n = g_light.size();
  for (int i = 0; i < 20; i++) tick(200);
  CHECK(g_light.size() == n, "mesma leitura: so candidata");
  tick(10000);
  CHECK(g_light.size() == n + 1 && g_light.back().on, "leitura seguinte confirma");

  printf("\n%d verificacoes ok, %d falharam\n", g_ok, g_fail);
  return g_fail ? 1 : 0;
}
