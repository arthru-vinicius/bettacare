// Teste de host de fan.cpp (o arquivo real, incluído inteiro): o corte de
// energia soltando a linha de PWM, e o potenciômetro — resposta na mesma volta,
// comando tomado por movimento, e ruído ou deriva que não tomam.
#include <string>
#include <vector>

#include "fan.cpp"   // o do firmware, via -I

uint32_t g_millis = 1000;

// O que o fan.cpp chama de outros módulos.
bool  temperature_is_fresh() { return true; }
float temperature_read() { return 26.0f; }   // abaixo do gatilho: automático parado
DeviceConfig device_config_snapshot() {
  DeviceConfig c = {};
  c.fan_trigger_c = 29.0f;
  c.fan_off_c = 27.5f;
  return c;
}
static std::vector<std::string> g_events;
void event_log(EventSeverity, EventComponent, const char* code, const char*, ...) {
  g_events.push_back(code);
}

static int g_fail = 0, g_ok = 0;
#define CHECK(cond, what)                                              \
  do {                                                                 \
    if (cond) { g_ok++; }                                              \
    else { g_fail++; printf("  FALHOU: %s  (linha %d)\n", what, __LINE__); } \
  } while (0)

static int quantos(const char* code) {
  int n = 0;
  for (auto& e : g_events) n += (e == code);
  return n;
}

/** Uma volta do loop de controle (200 ms). */
static void volta(int n = 1) {
  for (int i = 0; i < n; i++) {
    g_millis += 200;
    fan_update();
  }
}

static bool energia() { return g_pin[PIN_FAN_POWER] == HIGH; }
static int duty() { return g_ledc[PIN_FAN]; }
static bool manual() { return fan_get_mode_report() == FAN_REPORT_MANUAL; }
static bool automatico() { return fan_get_mode_report() == FAN_REPORT_AUTO; }

int main() {
  g_adc = 2000;
  fan_init();

  printf("V1 desligada: energia cortada E a linha de PWM solta (buffer desligado)\n");
  volta();
  CHECK(!energia(), "energia cortada");
  CHECK(duty() == 0, "duty 0: o MOSFET do PWM nao prende o pino 4 no GND");

  printf("V2 comando do app: liga com o complemento, desliga soltando a linha\n");
  fan_set_speed(50);
  CHECK(energia() && duty() == 127, "50% chega como 127 (complemento de 50%)");
  fan_set_speed(0);
  CHECK(!energia() && duty() == 0, "desligada pelo app: linha solta");

  printf("P1 pot parado depois de um comando: nao assume\n");
  fan_set_mode_auto(true);
  g_events.clear();
  volta(10);
  CHECK(automatico() && !energia(), "segue no automatico");
  CHECK(quantos("pot.fan_speed") == 0 && quantos("pot.fan_off") == 0, "nenhum evento do pot");

  printf("P2 girou: assume em 2 voltas (400 ms), sem precisar ir ao minimo\n");
  g_adc = 3000;
  volta(2);
  CHECK(manual(), "manual pelo pot");
  CHECK(fan_get_speed_percent() == 72 && energia() && duty() == 71, "72% na hora");
  CHECK(quantos("pot.fan_speed") == 0, "evento ainda nao: a mao pode estar girando");

  printf("P3 giro continuo: velocidade acompanha a cada volta, um evento so no fim\n");
  for (int adc = 3100; adc <= 4000; adc += 100) {
    g_adc = adc;
    volta();
  }
  // map(4000, 110, 4095, 1, 100) = 3890 * 99 / 3985 + 1 = 97
  CHECK(fan_get_speed_percent() == 97, "acompanhou ate 97%");
  CHECK(quantos("pot.fan_speed") == 0, "nenhum evento durante o giro");
  volta(5);
  CHECK(quantos("pot.fan_speed") == 1, "um evento quando a mao parou");

  printf("P4 minimo: desliga na volta seguinte, com a linha solta\n");
  g_adc = 50;
  volta();
  CHECK(fan_get_mode_report() == FAN_REPORT_MANUAL_OFF, "desligada pelo pot");
  CHECK(!energia() && duty() == 0, "energia cortada e linha solta");
  CHECK(quantos("pot.fan_off") == 1, "evento pot.fan_off");

  printf("P5 subindo do minimo: o pot segue no comando\n");
  g_adc = 1500;
  volta();
  CHECK(manual() && energia(), "religou pelo pot, sem precisar de nada");

  printf("P6 voltar ao automatico pelo app: o pot parado nao desfaz\n");
  fan_set_mode_auto(true);
  volta(10);
  CHECK(automatico(), "automatico mantido");

  printf("P7 pico de ruido isolado no ADC: nao assume\n");
  g_adc = 1800;
  volta();
  g_adc = 1500;
  volta(5);
  CHECK(automatico(), "uma amostra fora nao basta");

  printf("P8 deriva lenta do ADC (2 pontos por volta, 30 s): nao assume\n");
  for (int i = 0; i < 150; i++) {
    g_adc += 2;
    volta();
  }
  CHECK(automatico(), "a referencia acompanha a deriva");

  printf("P9 depois disso, um giro de verdade assume\n");
  g_adc += 800;
  volta(2);
  CHECK(manual(), "assumiu");

  printf("\n%d verificacoes ok, %d falharam\n", g_ok, g_fail);
  return g_fail ? 1 : 0;
}
