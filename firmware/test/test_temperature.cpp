// Teste de host de temperature.cpp (o arquivo real, incluído inteiro): leitura
// impossível, salto não confirmado, 85 °C de power-on e sensor perdido.
#include <string>
#include <vector>

#include "temperature.cpp"   // o do firmware, via -I

uint32_t g_millis = 1000;

struct Evento {
  std::string code;
  std::string msg;
};
static std::vector<Evento> g_events;
void event_log(EventSeverity, EventComponent, const char* code, const char* fmt, ...) {
  char b[200];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof b, fmt, a);
  va_end(a);
  g_events.push_back({code, b});
}

static int g_fail = 0, g_ok = 0;
#define CHECK(cond, what)                                              \
  do {                                                                 \
    if (cond) { g_ok++; }                                              \
    else { g_fail++; printf("  FALHOU: %s  (linha %d)\n", what, __LINE__); } \
  } while (0)

static bool tem(const char* code) {
  for (auto& e : g_events) if (e.code == code) return true;
  return false;
}

/** Uma conversão inteira: dispara (se o espaçamento deixar), espera 750 ms, lê. */
static void conversao(float c) {
  g_ds.set_celsius(c);
  temperature_update();
  g_millis += 800;
  temperature_update();
}

static void espera(uint32_t ms) { g_millis += ms; }

int main() {
  temperature_init();
  CHECK(temperature_available(), "sensor encontrado na sondagem");

  printf("C1 primeira leitura do boot aceita na hora\n");
  conversao(27.125f);
  CHECK(temperature_has_valid_reading() && temperature_read() == 27.125f, "27,125");

  printf("C2 -48,00 (0xFD00, o de producao): descartada, com o scratchpad no evento\n");
  espera(5000);
  conversao(-48.0f);
  CHECK(temperature_read() == 27.125f, "valor anterior mantido");
  CHECK(tem("temp.implausible"), "evento");
  CHECK(!g_events.empty() && g_events.back().msg.find("[00 FD") != std::string::npos,
        "scratchpad cru na mensagem");

  printf("C3 de volta ao normal: a falha isolada nao acumula\n");
  espera(5000);
  conversao(27.1875f);
  CHECK(temperature_read() == 27.1875f, "aceita");

  printf("C4 salto para 20 C: segura, rele na hora e descarta se nao confirmar\n");
  g_events.clear();
  espera(5000);
  int antes = g_ds.conversions;
  conversao(20.0f);
  CHECK(temperature_read() == 27.1875f, "salto segurado");
  conversao(27.25f);   // sem esperar os 5 s
  CHECK(g_ds.conversions == antes + 2, "releitura imediata");
  CHECK(temperature_read() == 27.25f, "releitura aceita");
  CHECK(tem("temp.spike_discarded"), "evento do salto");

  printf("C5 mudanca real de 3 C: confirmada pela releitura\n");
  g_events.clear();
  espera(5000);
  conversao(30.5f);
  CHECK(temperature_read() == 27.25f, "primeiro, segura");
  conversao(30.4375f);
  CHECK(temperature_read() == 30.4375f, "confirmada");
  CHECK(!tem("temp.spike_discarded"), "sem evento de salto");

  printf("C6 85,0 de power-on: descartado com o evento proprio\n");
  g_events.clear();
  espera(5000);
  conversao(85.0f);
  CHECK(tem("temp.reset_value") && !tem("temp.implausible"), "reset_value, nao implausible");
  CHECK(temperature_read() == 30.4375f, "mantido");

  printf("C7 tres leituras impossiveis seguidas: sensor perdido\n");
  espera(5000);
  conversao(30.5f);   // boa: zera a contagem
  g_events.clear();
  espera(5000);
  conversao(-48.0f);
  espera(5000);
  conversao(127.9375f);   // 0x07FF, o de alimentacao insuficiente
  CHECK(temperature_available(), "duas ainda nao derrubam");
  espera(5000);
  conversao(-48.0f);
  CHECK(!temperature_available() && !temperature_has_valid_reading(), "tres: perdido");
  CHECK(tem("temp.sensor_lost"), "evento sensor_lost");

  printf("C8 reencontrado; CRC ruim isolado sem evento\n");
  espera(10000);
  temperature_update();   // sondagem
  CHECK(temperature_available(), "reencontrado");
  conversao(26.0f);
  CHECK(temperature_read() == 26.0f, "primeira leitura depois de reencontrar, sem confirmacao");
  g_events.clear();
  espera(5000);
  g_ds.crc_ok = false;
  conversao(26.0f);
  g_ds.crc_ok = true;
  CHECK(g_events.empty() && temperature_read() == 26.0f, "CRC isolado: nada muda");

  printf("\n%d verificacoes ok, %d falharam\n", g_ok, g_fail);
  return g_fail ? 1 : 0;
}
