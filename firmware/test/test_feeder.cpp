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
  feeder_link_request_feed(0, false);
  feeder_link_request_feed(4, false);
  feeder_link_request_config(9, 21, 6, true);
  CHECK(Serial2.out == "FEED\nFEED 4\nCFG 9 21 6 1\n", "formato FEED/CFG");
  Serial2.out.clear();
  feeder_link_request_feed(0, true);
  feeder_link_request_feed(4, true);
  CHECK(Serial2.out == "FEED FORCE\nFEED 4 FORCE\n", "FORCE so quando o app ignora o limite");

  printf("F11b refeicoes em 24 h no PONG; modulo antigo sem o campo segue entendido\n");
  CHECK(feeder_link_get_state().meals_24h == FEEDER_MEALS_UNKNOWN, "desconhecido antes");
  feed("PONG 8 20 5 1 60 5 5 1 2\n");
  CHECK(feeder_link_get_state().meals_24h == 2, "2 refeicoes");
  feed("PONG 8 20 5 1 60 5 5 1\n");
  CHECK(feeder_link_get_state().meals_24h == 2 && feeder_link_get_state().connected,
        "sem o 9o campo: mantem o ultimo e segue conectado");

  printf("F11c DENIED: limite e ocupado viram eventos proprios\n");
  g_events.clear();
  feed("DENIED LIMITE 3 BOTAO\n");
  CHECK(has("feeder.limit_reached") && feeder_link_get_state().meals_24h == 3, "limite, 3 refeicoes");
  feed("DENIED OCUPADO 1 APP\n");
  CHECK(has("feeder.feed_denied"), "ocupado");
  g_events.clear();
  feed("DENIED CALIBRAR 0 BOTAO\n");
  feed("DENIED NOVIDADE 0 APP\n");
  CHECK(g_events.size() == 2, "calibrar e motivo desconhecido tambem avisam");
  g_events.clear();
  feed("DENIED PERDIDA 1 AGENDA\n");
  CHECK(has("feeder.meal_missed"), "refeicao perdida tem evento proprio");
  g_events.clear();
  feed("DENIED LIMITE x BOTAO\n");
  CHECK(g_events.empty(), "contagem corrompida: linha descartada");

  printf("F11d FED com motivo: reservatorio vazio e sensor com defeito\n");
  g_events.clear();
  feed("FED 5 0 0 VAZIO AGENDA\n");
  CHECK(has("feeder.hopper_empty"), "vazio");
  feed("FED 5 0 0 SENSOR BOTAO\n");
  CHECK(has("feeder.sensor_fault"), "sensor");
  g_events.clear();
  feed("FED 5 5 1 OKMUITOCOMPRIDO AGENDA\n");
  CHECK(has("feeder.fed_ok"), "motivo comprido demais: fica vazio, sem lixo");

  // Um ciclo do loop: o feeder_link é chamado a cada ~200 ms.
  auto ciclos = [](int n) {
    for (int i = 0; i < n; i++) {
      g_millis += 200;
      feeder_link_update();
    }
  };

  printf("F12 conector vazio (pull-down): ausente e nenhum PING\n");
  feeder_link_init();
  g_gpio_level = 0;
  Serial2.out.clear();
  ciclos(20);
  CHECK(!feeder_link_present(), "ausente");
  CHECK(Serial2.out.empty(), "nenhum PING para ninguem");

  printf("F13 modulo plugado: presente em duas amostras, PING a cada 2 s\n");
  g_gpio_level = 1;
  ciclos(1);
  CHECK(!feeder_link_present(), "uma amostra alta nao basta");
  ciclos(1);
  CHECK(feeder_link_present(), "duas: presente");
  CHECK(Serial2.out == "PING\n", "primeiro PING");
  ciclos(10);
  CHECK(Serial2.out == "PING\nPING\n", "um PING a cada 2 s");

  printf("F14 cabo desligado com o modulo conectado: cai na hora, sem esperar 6 s\n");
  feed("PONG 8 20 5 0 60 3 3 1\n");
  CHECK(feeder_link_get_state().connected, "conectado");
  g_events.clear();
  g_gpio_level = 0;
  ciclos(4);
  CHECK(feeder_link_get_state().connected, "quatro amostras baixas ainda podem ser bits");
  ciclos(1);
  CHECK(!feeder_link_present() && !feeder_link_get_state().connected, "cinco: ausente e desconectado");
  CHECK(has("feeder.module_disconnected"), "evento disconnected");
  Serial2.out.clear();
  ciclos(20);
  CHECK(Serial2.out.empty(), "e nenhum PING depois");

  printf("F15 o 0x00 do break nao estraga a linha seguinte\n");
  g_gpio_level = 1;
  Serial2.in += std::string(1, '\0') + "PONG 6 18 2 1 -1 0 0 0\n";
  feeder_link_update();
  s = feeder_link_get_state();
  CHECK(s.connected && s.hour1 == 6 && s.grains_per_feeding == 2, "PONG depois do 0x00 aplicado");
  CHECK(feeder_link_present(), "linha valida prova presenca");

  printf("\n%d verificacoes ok, %d falharam\n", g_ok, g_fail);
  return g_fail ? 1 : 0;
}
