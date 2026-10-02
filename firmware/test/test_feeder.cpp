// Teste de host do parser de feeder_link.cpp (o arquivo real, incluído inteiro).
#include <string>
#include <vector>

#include "feeder_link.cpp"   // o do firmware, via -I

uint32_t g_millis = 1000;
FakeSerial Serial, Serial2;

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

static void feed(const char* line) {
  Serial2.in += line;
  feeder_link_update();
}
static bool has(const char* code) {
  for (auto& e : g_events) if (e == code) return true;
  return false;
}

int main() {
  feeder_link_init();

  printf("F1 PONG valido, nunca alimentou (-1)\n");
  feed("PONG 10 18 3 1 -1 0 0 0\n");
  FeederLinkState s = feeder_link_get_state();
  CHECK(s.connected, "conectado");
  CHECK(s.hour1 == 10 && s.hour2 == 18 && s.grains_per_feeding == 3 && s.auto_enabled, "agenda");
  CHECK(s.last_feed_age_s == UINT32_MAX, "nunca alimentou");

  printf("F2 PONG com hora 25: descartado inteiro\n");
  feed("PONG 25 18 3 1 5 3 3 1\n");
  s = feeder_link_get_state();
  CHECK(s.hour1 == 10 && s.last_feed_age_s == UINT32_MAX, "estado intacto");

  printf("F3 SCHEDULE valido\n");
  feed("SCHEDULE 8 20 5 0\n");
  s = feeder_link_get_state();
  CHECK(s.hour1 == 8 && s.hour2 == 20 && s.grains_per_feeding == 5 && !s.auto_enabled, "agenda nova");
  CHECK(has("feeder.config_applied"), "evento config_applied");

  printf("F4 SCHEDULE com digito corrompido, graos 0 e graos 21: descartados\n");
  g_events.clear();
  feed("SCHEDULE 8 2x 5 0\n");
  feed("SCHEDULE 8 20 0 1\n");
  feed("SCHEDULE 8 20 21 1\n");
  feed("SCHEDULE 8 20 5 2\n");
  feed("SCHEDULE 8 20 5\n");
  s = feeder_link_get_state();
  CHECK(s.hour2 == 20 && s.grains_per_feeding == 5 && !s.auto_enabled, "estado intacto");
  CHECK(!has("feeder.config_applied"), "nenhum evento");

  printf("F5 FED valido e incompleto\n");
  feed("FED 3 2 0\n");
  s = feeder_link_get_state();
  CHECK(s.last_feed_requested == 3 && s.last_feed_confirmed == 2 && !s.last_feed_ok, "resultado");
  CHECK(s.last_feed_age_s == 0, "idade zerada");
  CHECK(has("feeder.fed_incomplete"), "evento fed_incomplete");

  printf("F6 FED fora de faixa: descartado\n");
  g_events.clear();
  feed("FED 30 2 0\n");
  feed("FED 3 -2 0\n");
  s = feeder_link_get_state();
  CHECK(s.last_feed_requested == 3 && s.last_feed_confirmed == 2, "intacto");
  CHECK(g_events.empty(), "sem eventos");

  printf("F7 PONG com idade, e idade gigante descartada\n");
  feed("PONG 8 20 5 0 120 3 2 0\n");
  s = feeder_link_get_state();
  CHECK(s.last_feed_age_s == 120, "idade 120");
  feed("PONG 8 20 5 0 99999999999 3 2 0\n");
  s = feeder_link_get_state();
  CHECK(s.last_feed_age_s == 120, "idade absurda ignorada");

  printf("F8 idade envelhece em tempo real entre PONGs\n");
  g_millis += 5000;
  s = feeder_link_get_state();
  CHECK(s.last_feed_age_s == 125, "120 + 5 s");

  printf("F9 timeout desconecta; lixo nao reconecta; PONG reconecta\n");
  g_events.clear();
  g_millis += 7000;
  feeder_link_update();
  CHECK(!feeder_link_get_state().connected, "desconectado por timeout");
  CHECK(has("feeder.module_disconnected"), "evento disconnected");
  feed("XYZ 1 2 3\n");
  feed("PONGX 1 2\n");
  CHECK(!feeder_link_get_state().connected, "linha desconhecida nao reconecta");
  feed("PONG 8 20 5 0 60 3 3 1\n");
  CHECK(feeder_link_get_state().connected, "PONG reconecta");
  CHECK(has("feeder.module_connected"), "evento connected na reconexao");

  printf("F10 linha gigante e CR/LF\n");
  std::string longa(300, '9');
  feed(("SCHEDULE " + longa + "\r\n").c_str());
  feed("SCHEDULE 7 19 4 1\r\n");
  s = feeder_link_get_state();
  CHECK(s.hour1 == 7 && s.hour2 == 19 && s.grains_per_feeding == 4 && s.auto_enabled,
        "linha gigante descartada, a seguinte (CRLF) aplicada");

  printf("F10b campo maior que o buffer nao vira dois campos\n");
  feed("SCHEDULE 00000001 5 1\n");
  s = feeder_link_get_state();
  CHECK(s.hour1 == 7 && s.hour2 == 19, "linha com campo longo descartada");

  printf("F11 comandos de saida\n");
  Serial2.out.clear();
  feeder_link_request_feed(0);
  feeder_link_request_feed(4);
  feeder_link_request_config(9, 21, 6, true);
  CHECK(Serial2.out == "FEED\nFEED 4\nCFG 9 21 6 1\n", "formato FEED/CFG");

  printf("\n%d verificacoes ok, %d falharam\n", g_ok, g_fail);
  return g_fail ? 1 : 0;
}
