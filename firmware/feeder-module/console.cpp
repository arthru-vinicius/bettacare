#include "console.h"

#include <Arduino.h>
#include <WiFi.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "clock.h"
#include "config.h"
#include "debuglog.h"
#include "doser.h"
#include "link.h"
#include "ota_manager.h"
#include "protocol.h"
#include "ui.h"

static const uint16_t FEIXE_PADRAO_S = 15;

static char    _linha[80];
static uint8_t _len = 0;
static bool    _descartando = false;   // linha longa demais: ignora até o fim

// Teste do feixe em andamento: o console narra o que passa.
static bool     _narrando = false;
static uint16_t _narrando_s = 0;
static uint16_t _visto = 0;

// Um comando vindo da rede (POST /console), escrito pela tarefa do servidor e
// executado aqui, no loop — o dono do doseador, da NVS e da tela.
static portMUX_TYPE  _mux = portMUX_INITIALIZER_UNLOCKED;
static char          _remoto[sizeof(_linha)];
static volatile bool _tem_remoto = false;

static void _ajuda() {
  Log.println(
      "Comandos (um por linha):\n"
      "  estado                       tudo o que o modulo sabe agora\n"
      "  hora                         mostra a hora\n"
      "  hora AAAA-MM-DD HH:MM[:SS]   acerta o relogio (hora do aquario)\n"
      "  agenda                       mostra a agenda\n"
      "  agenda H1 H2 GRAOS AUTO      ex.: agenda 5 17 5 1 (AUTO: 1 liga, 0 desliga)\n"
      "  alimentar [GRAOS]            como o botao: conta no limite de 3 em 24 h\n"
      "  alimentar [GRAOS] forcar     passa do limite, como o app\n"
      "  calibrar                     ajuste pelo botao: 1x +2, 3x -2, 2x troca, segurar salva\n"
      "  calibrar REPOUSO DESPEJO     grava os angulos direto (graus)\n"
      "  servo GRAUS                  leva o slide ate la, devagar, e solta o servo\n"
      "  teste m1                     pulso de 2 s do motor anti-empacamento\n"
      "  teste m2                     o aviso ao peixe, sem racao\n"
      "  teste sensor                 autoteste do par IR\n"
      "  teste feixe [SEGUNDOS]       autoteste e conta o que cortar o feixe (padrao 15 s)\n"
      "  teste grao                   uma dose de 1 grao, sem contar como refeicao\n"
      "                               (despeja de verdade: copo sob o bico)\n"
      "  reiniciar\n"
      "  fabrica sim                  apaga agenda, calibracao e historico");
}

/** Inteiro decimal, sem sobra, dentro de [min, max]. */
static bool _num(const char* s, long min, long max, long& out) {
  if (!s || !*s) return false;
  char* fim = nullptr;
  long v = strtol(s, &fim, 10);
  if (*fim != '\0' || v < min || v > max) return false;
  out = v;
  return true;
}

static const char* _fase(DoserPhase f) {
  switch (f) {
    case DoserPhase::IDLE:         return "parado";
    case DoserPhase::WARNING:      return "refeicao: aviso ao peixe";
    case DoserPhase::SELFTEST:     return "refeicao: autoteste do sensor";
    case DoserPhase::DOSING:       return "refeicao: dosando";
    case DoserPhase::FINISHING:    return "refeicao: terminando";
    case DoserPhase::M1_PULSE:     return "pulso do M1";
    case DoserPhase::WARNING_TEST: return "teste do aviso";
    case DoserPhase::BEAM_TEST:    return "teste do feixe";
  }
  return "?";
}

static void _imprime_hora() {
  if (!clock_valid()) {
    Log.println("Hora: sem hora valida (DS3231 sem bateria e sem NTP?) - a agenda esta parada");
    return;
  }
  CivilTime t = epoch_to_civil(clock_now());
  Log.printf("Hora: %04u-%02u-%02u %02u:%02u:%02u (DS3231 %s, NTP %s)\n", t.year, t.month, t.day,
                t.hour, t.minute, t.second, clock_rtc_present() ? "ok" : "ausente",
                clock_ntp_synced() ? "ok" : "--");
}

static void _imprime_agenda() {
  const FeederConfig& c = app_config();
  Log.printf("Agenda: %02u h e %02u h, %u graos, automatico %s\n", c.hour1, c.hour2, c.grains,
                c.auto_enabled ? "ligado" : "desligado");
  uint32_t prox = app_next_meal_at();
  if (prox) {
    CivilTime t = epoch_to_civil(prox);
    Log.printf("Proxima: %02u:%02u\n", t.hour, t.minute);
  }
}

static void _estado() {
  Log.printf("%s, fw %s, ligado ha %lu s\n", DEVICE_ID, FW_VERSION, (unsigned long)(millis() / 1000));
  _imprime_hora();
  if (WiFi.status() == WL_CONNECTED) {
    Log.printf("Wi-Fi: %s, %d dBm\n", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  } else {
    Log.println("Wi-Fi: desconectado");
  }
  Log.printf("Principal: %s\n", link_connected() ? "no fio" : "fora do fio");
  _imprime_agenda();

  const Calibration& k = app_calibration();
  Log.printf("Calibracao: repouso %u, despejo %u%s\n", k.rest_deg, k.dump_deg,
                k.valid ? "" : " - NAO CALIBRADO: nenhuma refeicao sai");
  Log.printf("Refeicoes em 24 h: %u de %u\n", app_meals_24h(), MEALS_PER_24H);

  uint32_t idade;
  const History& h = app_history();
  if (app_last_feed_age(idade)) {
    Log.printf("Ultima refeicao: ha %lu min, %u de %u graos, %s\n", (unsigned long)(idade / 60),
                  h.last_conf, h.last_req, reason_token((MealReason)h.last_reason));
  } else {
    Log.println("Ultima refeicao: nenhuma");
  }

  if (!doser_sensor_tested()) {
    Log.println("Sensor: nao testado ainda");
  } else {
    Log.printf("Sensor: %s no ultimo autoteste\n", doser_sensor_ok() ? "ok" : "REPROVADO");
  }
  Log.printf("Doseador: %s\n", _fase(doser_phase()));
  Log.printf("Aviso pendente (LED piscando): %s\n", app_has_error() ? "sim" : "nao");
  Log.printf("Tela: %s\n", ui_has_display() ? "ok" : "ausente");
  if (ui_calibrating()) Log.println("Em calibracao pelo botao");
  if (ota_in_progress()) Log.println("Atualizacao OTA em andamento");
}

static void _hora(char* args) {
  if (!args || !*args) return _imprime_hora();
  unsigned a = 0, me = 0, d = 0, h = 0, mi = 0, s = 0;
  int n = sscanf(args, "%u-%u-%u %u:%u:%u", &a, &me, &d, &h, &mi, &s);
  if (n < 5 || a > 9999 || me > 12 || d > 31 || h > 23 || mi > 59 || s > 59) {
    Log.println("Uso: hora AAAA-MM-DD HH:MM[:SS]  (ex.: hora 2026-10-02 18:30)");
    return;
  }
  CivilTime t = {(uint16_t)a, (uint8_t)me, (uint8_t)d, (uint8_t)h, (uint8_t)mi, (uint8_t)s};
  if (!civil_valid(t)) {
    Log.println("Data invalida (ano 2024 a 2099, dia que exista no mes)");
    return;
  }
  if (!clock_set(t)) {
    Log.println("Falha: o DS3231 nao gravou a hora");
    return;
  }
  _imprime_hora();
}

static void _agenda(char* args) {
  char* r = nullptr;
  char* t1 = strtok_r(args, " \t", &r);
  if (!t1) return _imprime_agenda();
  char* t2 = strtok_r(nullptr, " \t", &r);
  char* t3 = strtok_r(nullptr, " \t", &r);
  char* t4 = strtok_r(nullptr, " \t", &r);
  long h1, h2, g, a;
  if (!_num(t1, 0, 23, h1) || !_num(t2, 0, 23, h2) || !_num(t3, GRAINS_MIN, GRAINS_MAX, g) ||
      !_num(t4, 0, 1, a) || strtok_r(nullptr, " \t", &r)) {
    Log.printf("Uso: agenda H1 H2 GRAOS AUTO  (horas 0-23, graos %u-%u, AUTO 0 ou 1)\n", GRAINS_MIN,
                  GRAINS_MAX);
    return;
  }
  FeederConfig c = {(uint8_t)h1, (uint8_t)h2, (uint8_t)g, a == 1};
  if (app_save_config(c)) {
    Log.println("Agenda gravada: vale da proxima ocorrencia em diante");
  } else {
    Log.println("Agenda invalida; nada mudou");
  }
}

static void _alimentar(char* args) {
  long g = 0;
  bool forcar = false;
  char* r = nullptr;
  for (char* t = strtok_r(args, " \t", &r); t; t = strtok_r(nullptr, " \t", &r)) {
    if (!strcmp(t, "forcar")) {
      forcar = true;
    } else if (!_num(t, GRAINS_MIN, GRAINS_MAX, g)) {
      Log.printf("Uso: alimentar [GRAOS %u-%u] [forcar]\n", GRAINS_MIN, GRAINS_MAX);
      return;
    }
  }
  // Recusa e resultado saem pelo próprio app ("[Refeicao] ...").
  app_feed((uint8_t)g, forcar ? Origin::FORCADO : Origin::BOTAO);
}

static void _calibrar(char* args) {
  char* r = nullptr;
  char* t1 = strtok_r(args, " \t", &r);
  if (!t1) {
    if (!ui_begin_calibration()) {
      Log.println("Ocupado (refeicao, teste ou OTA); tente de novo");
      return;
    }
    Log.println(
        "Calibracao pelo botao. O slide foi para o repouso atual.\n"
        "  1x +2 graus, 3x -2 graus, 2x alterna repouso/despejo, segurar 2 s salva.\n"
        "  Repouso: bolso inteiro sob o reservatorio. Despejo: bolso inteiro sobre o tubo.\n"
        "  2 min sem tocar cancela.");
    return;
  }
  char* t2 = strtok_r(nullptr, " \t", &r);
  long rep, desp;
  if (!_num(t1, 0, 180, rep) || !_num(t2, 0, 180, desp) || strtok_r(nullptr, " \t", &r)) {
    Log.println("Uso: calibrar  ou  calibrar REPOUSO DESPEJO  (graus, 0-180)");
    return;
  }
  if (labs(rep - desp) < CAL_MIN_TRAVEL_DEG) {
    Log.printf("Curso curto demais: repouso e despejo precisam de %u graus de distancia\n",
                  CAL_MIN_TRAVEL_DEG);
    return;
  }
  if (ui_calibrating()) {
    Log.println("Calibracao pelo botao em andamento; termine por la");
    return;
  }
  if (doser_busy() || ota_in_progress()) {
    Log.println("Ocupado (refeicao, teste ou OTA); tente de novo");
    return;
  }
  Calibration k = {(uint8_t)rep, (uint8_t)desp, true};
  app_save_calibration(k);
  app_move_servo(k.rest_deg);
  Log.println("Calibracao gravada; o slide vai para o repouso novo");
}

static void _servo(char* args) {
  char* r = nullptr;
  long deg;
  if (!_num(strtok_r(args, " \t", &r), 0, 180, deg) || strtok_r(nullptr, " \t", &r)) {
    Log.println("Uso: servo GRAUS  (0-180)");
    return;
  }
  if (!app_move_servo((uint8_t)deg)) {
    Log.println("Ocupado (refeicao, teste, calibracao ou OTA); tente de novo");
    return;
  }
  Log.printf("Slide indo para %ld graus; o servo solta ao chegar\n", deg);
}

static void _teste(char* args) {
  char* r = nullptr;
  char* qual = strtok_r(args, " \t", &r);
  char* arg = strtok_r(nullptr, " \t", &r);
  bool ok;
  if (qual && !strcmp(qual, "m1")) {
    ok = app_test_m1();
    if (ok) Log.println("M1 por 2 s");
  } else if (qual && (!strcmp(qual, "m2") || !strcmp(qual, "aviso"))) {
    ok = app_test_warning();
    if (ok) Log.println("Aviso ao peixe: rampa de 1 s, 2 s firme, rampa de 1 s");
  } else if (qual && (!strcmp(qual, "sensor") || !strcmp(qual, "feixe"))) {
    long s = 0;
    if (!strcmp(qual, "feixe")) {
      s = FEIXE_PADRAO_S;
      if (arg && !_num(arg, 1, BEAM_TEST_MAX_S, s)) {
        Log.printf("Uso: teste feixe [SEGUNDOS 1-%u]\n", BEAM_TEST_MAX_S);
        return;
      }
    }
    ok = app_test_beam((uint16_t)s);
    if (ok) {
      _narrando = true;
      _narrando_s = (uint16_t)s;
      _visto = 0;
      if (s) Log.printf("Autoteste e %ld s contando: passe graos pelo feixe\n", s);
    }
  } else if (qual && !strcmp(qual, "grao")) {
    if (!app_calibration().valid) {
      Log.println("Doseador sem calibracao: calibre antes (comando calibrar)");
      return;
    }
    ok = app_test_grain();
    if (ok) Log.println("Dose de 1 grao (aviso, autoteste, M1, slide); nao conta como refeicao");
  } else {
    Log.println("Uso: teste m1 | m2 | sensor | feixe [SEGUNDOS] | grao");
    return;
  }
  if (!ok) Log.println("Ocupado (refeicao, teste, calibracao ou OTA); tente de novo");
}

static void _narra_feixe() {
  if (!_narrando) return;
  if (doser_phase() == DoserPhase::BEAM_TEST) {
    uint16_t n = doser_beam_count();
    if (n != _visto) {
      _visto = n;
      Log.printf("[Feixe] %u\n", n);
    }
    return;
  }
  _narrando = false;
  if (!doser_sensor_ok()) {
    Log.println(
        "[Feixe] Autoteste REPROVADO: o LM393 leu o mesmo nivel com o LED IR apagado e aceso\n"
        "        (ou oscilou). Confira: LED IR ligando, par alinhado, ajuste do potenciometro\n"
        "        do LM393, luz de fora entrando, grao preso no feixe.");
  } else if (_narrando_s == 0) {
    Log.println("[Feixe] Autoteste ok: apagado le bloqueado, aceso le livre");
  } else {
    Log.printf("[Feixe] Fim: %u passaram em %u s\n", doser_beam_count(), _narrando_s);
  }
}

static void _executa(char* linha) {
  char* r = nullptr;
  char* cmd = strtok_r(linha, " \t", &r);
  if (!cmd) return;
  for (char* p = cmd; *p; p++) *p = (char)tolower((unsigned char)*p);
  if (!strcmp(cmd, "ajuda") || !strcmp(cmd, "?")) {
    _ajuda();
  } else if (!strcmp(cmd, "estado")) {
    _estado();
  } else if (!strcmp(cmd, "hora")) {
    _hora(r);
  } else if (!strcmp(cmd, "agenda")) {
    _agenda(r);
  } else if (!strcmp(cmd, "alimentar")) {
    _alimentar(r);
  } else if (!strcmp(cmd, "calibrar")) {
    _calibrar(r);
  } else if (!strcmp(cmd, "servo")) {
    _servo(r);
  } else if (!strcmp(cmd, "teste")) {
    _teste(r);
  } else if (!strcmp(cmd, "reiniciar")) {
    doser_abort();
    Log.println("Reiniciando...");
    Serial.flush();
    delay(100);
    ESP.restart();
  } else if (!strcmp(cmd, "fabrica")) {
    char* conf = strtok_r(nullptr, " \t", &r);
    if (conf && !strcmp(conf, "sim")) {
      app_factory_reset();
    } else {
      Log.println("Apaga agenda, calibracao e historico (o limite de 24 h recomeca). Confirme: fabrica sim");
    }
  } else {
    Log.printf("Comando desconhecido: '%s' (ajuda lista os comandos)\n", cmd);
  }
}

bool console_enqueue(const char* line) {
  if (!line || !*line || strlen(line) >= sizeof(_remoto)) return false;
  bool ok = false;
  portENTER_CRITICAL(&_mux);
  if (!_tem_remoto) {
    strcpy(_remoto, line);
    _tem_remoto = true;
    ok = true;
  }
  portEXIT_CRITICAL(&_mux);
  return ok;
}

/** O comando vai para o log antes da saída dele: lendo de longe, dá para saber o que gerou o quê. */
static void _roda(char* linha, const char* origem) {
  Log.printf("%s %s\n", origem, linha);
  _executa(linha);
}

void console_update() {
  if (_tem_remoto) {
    char linha[sizeof(_remoto)];
    portENTER_CRITICAL(&_mux);
    memcpy(linha, _remoto, sizeof(linha));
    _tem_remoto = false;
    portEXIT_CRITICAL(&_mux);
    _roda(linha, "[rede]>");
  }
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      bool tinha = _len > 0 && !_descartando;
      _linha[_len] = '\0';
      _len = 0;
      _descartando = false;
      if (tinha) _roda(_linha, "[usb]>");
      continue;
    }
    if (_descartando) continue;
    if (_len < sizeof(_linha) - 1) {
      _linha[_len++] = c;
    } else {
      _descartando = true;
      Log.println("Linha longa demais; ignorada");
    }
  }
  _narra_feixe();
}
