#include "ui.h"

#include <Arduino.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <Wire.h>

#include "app.h"
#include "button.h"
#include "clock.h"
#include "config.h"
#include "debuglog.h"
#include "doser.h"
#include "gestures.h"
#include "hw.h"
#include "link.h"
#include "ota_manager.h"

static const uint8_t  OLED_ADDR = 0x3C;
/** Abaixo do padrão (0xCF): legível ao lado do aquário, e o OLED gasta menos. */
static const uint8_t  OLED_CONTRAST = 128;
static const uint16_t REDRAW_MS = 250;
static const uint8_t  PAGINAS = 3;

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C _oled(U8G2_R0, U8X8_PIN_NONE, PIN_I2C_SCL, PIN_I2C_SDA);
static bool _tem_tela = false;
static GestureDetector _gestos;

enum class Modo : uint8_t { NORMAL, PROGRAMA, CALIBRA };
static Modo _modo = Modo::NORMAL;

static bool     _tela = false;      // deve estar acesa
static bool     _ligada = false;    // o painel está aceso de fato
static bool     _redesenhar = false;
static uint32_t _acesa_ate = 0;
static uint32_t _ultimo_desenho = 0;
static uint32_t _ultimo_toque = 0;
static uint8_t  _pagina = 0;

// Mensagem por cima de tudo, por alguns segundos.
static char     _msg1[32] = "";
static char     _msg2[32] = "";
static bool     _msg_ativa = false;
static uint32_t _msg_ate = 0;

// O que o LED piscando quer dizer — vale enquanto app_has_error().
static char _erro[32] = "";

// Programação: editada aqui, gravada só no "segurar".
static FeederConfig _edicao;
static uint8_t      _campo = 0;   // 0 hora 1, 1 hora 2, 2 grãos, 3 automático

// Calibração.
static Calibration _cal_edicao;
static uint8_t     _alvo = 0;     // 0 repouso, 1 despejo

static void _acender(uint32_t ms = SCREEN_ON_MS) {
  uint32_t ate = millis() + ms;
  if (!_tela || (int32_t)(ate - _acesa_ate) > 0) _acesa_ate = ate;
  _tela = true;
  _redesenhar = true;
}

static void _apagar() {
  if (_ligada) _oled.setPowerSave(1);
  _ligada = false;
  _tela = false;
  _pagina = 0;
}

static void _copia(char* dst, size_t n, const char* src) {
  strncpy(dst, src, n - 1);
  dst[n - 1] = '\0';
}

static void _mensagem(const char* l1, const char* l2, uint32_t ms) {
  _copia(_msg1, sizeof(_msg1), l1);
  _copia(_msg2, sizeof(_msg2), l2);
  _msg_ativa = true;
  _msg_ate = millis() + ms;
  _acender(ms);
}

void ui_init() {
  Wire.beginTransmission(OLED_ADDR);
  _tem_tela = Wire.endTransmission() == 0;
  if (_tem_tela) {
    _oled.setBusClock(400000);   // antes do begin(): vale já para a inicialização
    _oled.begin();
    _oled.setContrast(OLED_CONTRAST);
    _oled.setPowerSave(1);
  } else {
    Log.println("[Tela] SSD1306 nao responde em 0x3C; seguindo sem tela (LED e console)");
  }
  button_init();
}

// ── Estado do doseador, do ponto de vista da tela ────────────────────────────

static bool _em_refeicao() {
  switch (doser_phase()) {
    case DoserPhase::WARNING:
    case DoserPhase::SELFTEST:
    case DoserPhase::DOSING:
    case DoserPhase::FINISHING: return true;
    default:                    return false;
  }
}

/** Refeição ou teste da bancada: a tela fica acesa mostrando o andamento. */
static bool _em_atividade() { return doser_busy() && doser_phase() != DoserPhase::M1_PULSE; }

// ── Gestos ───────────────────────────────────────────────────────────────────

static void _sair_calibracao(bool salvar) {
  if (salvar) {
    int curso = (int)_cal_edicao.dump_deg - (int)_cal_edicao.rest_deg;
    if (curso < 0) curso = -curso;
    if (curso < CAL_MIN_TRAVEL_DEG) {
      _mensagem("Curso curto demais", "afaste os ângulos", 4000);
      return;   // segue calibrando
    }
    app_save_calibration(_cal_edicao);
    _mensagem("Calibração salva", "", 4000);
  } else {
    _mensagem("Calibração", "cancelada", 4000);
  }
  // O slide volta ao repouso em vigor — o novo, se salvou — e o servo solta.
  doser_servo_park(app_calibration().rest_deg);
  _modo = Modo::NORMAL;
}

static void _gesto_normal(Gesture g) {
  switch (g) {
    case Gesture::TAP1:
      if (_msg_ativa) {
        _msg_ativa = false;   // um toque dispensa a mensagem
      } else if (_tela) {
        _pagina = (uint8_t)((_pagina + 1) % PAGINAS);
      }
      _acender();
      break;
    case Gesture::TAP2:
      _msg_ativa = false;
      _pagina = 0;
      _acender();
      app_feed(0, Origin::BOTAO);
      break;
    case Gesture::TAP3:
      _msg_ativa = false;
      if (app_test_warning()) {
        _acender();
      } else {
        _mensagem("Ocupado", "tente de novo", 3000);
      }
      break;
    case Gesture::LONG:
      _msg_ativa = false;
      _edicao = app_config();
      _campo = 0;
      _modo = Modo::PROGRAMA;
      _acender();
      break;
    case Gesture::NONE:
      break;
  }
}

static void _ajusta_campo(int8_t delta) {
  switch (_campo) {
    case 0: _edicao.hour1 = (uint8_t)((_edicao.hour1 + 24 + delta) % 24); break;
    case 1: _edicao.hour2 = (uint8_t)((_edicao.hour2 + 24 + delta) % 24); break;
    case 2: {
      int g = _edicao.grains + delta;
      if (g < GRAINS_MIN) g = GRAINS_MAX;
      if (g > GRAINS_MAX) g = GRAINS_MIN;
      _edicao.grains = (uint8_t)g;
      break;
    }
    default: _edicao.auto_enabled = !_edicao.auto_enabled; break;
  }
}

static void _gesto_programa(Gesture g) {
  switch (g) {
    case Gesture::TAP1: _ajusta_campo(+1); break;
    case Gesture::TAP3: _ajusta_campo(-1); break;
    case Gesture::TAP2: _campo = (uint8_t)((_campo + 1) % 4); break;
    case Gesture::LONG:
      _modo = Modo::NORMAL;
      if (app_save_config(_edicao)) {
        _mensagem("Agenda salva", "vale da próxima", 4000);
      } else {
        _mensagem("Agenda inválida", "nada mudou", 4000);
      }
      break;
    case Gesture::NONE: break;
  }
  _acender();
}

static void _gesto_calibra(Gesture g) {
  uint8_t& deg = _alvo == 0 ? _cal_edicao.rest_deg : _cal_edicao.dump_deg;
  switch (g) {
    case Gesture::TAP1: deg = (uint8_t)(deg + CAL_STEP_DEG > 180 ? 180 : deg + CAL_STEP_DEG); break;
    case Gesture::TAP3: deg = (uint8_t)(deg < CAL_STEP_DEG ? 0 : deg - CAL_STEP_DEG); break;
    case Gesture::TAP2: _alvo ^= 1; break;
    case Gesture::LONG: _sair_calibracao(true); break;
    case Gesture::NONE: break;
  }
  if (_modo == Modo::CALIBRA) {
    // Na mesma velocidade da refeição: o ajuste também não dá tranco.
    doser_servo_hold(_alvo == 0 ? _cal_edicao.rest_deg : _cal_edicao.dump_deg);
  }
  _acender();
}

static void _gesto(Gesture g) {
  if (g == Gesture::NONE) return;
  _ultimo_toque = millis();
  if (app_has_error()) app_clear_error();   // qualquer toque reconhece o aviso
  switch (_modo) {
    case Modo::NORMAL:   _gesto_normal(g); break;
    case Modo::PROGRAMA: _gesto_programa(g); break;
    case Modo::CALIBRA:  _gesto_calibra(g); break;
  }
}

// ── Desenho ──────────────────────────────────────────────────────────────────
// Fonte 6×12: 21 caracteres por linha. Só Latim-1 (ç, ã, °, ×, ·): é o que as
// fontes "_tf" do U8g2 têm.

static void _texto(uint8_t y, const char* s) { _oled.drawUTF8(0, y, s); }

static void _centro(uint8_t y, const char* s) {
  int w = _oled.getUTF8Width(s);
  _oled.drawUTF8(w >= 128 ? 0 : (128 - w) / 2, y, s);
}

static void _hhmm(char* b, size_t n, uint32_t epoch) {
  CivilTime t = epoch_to_civil(epoch);
  snprintf(b, n, "%02u:%02u", t.hour, t.minute);
}

static void _pagina_estado() {
  char b[40];
  _oled.setFont(u8g2_font_logisoso24_tn);
  if (clock_valid()) {
    _hhmm(b, sizeof(b), clock_now());
  } else {
    strcpy(b, "--:--");
  }
  _oled.drawStr(0, 26, b);

  // Canto: refeições nas últimas 24 h, o número que o limite olha.
  _oled.setFont(u8g2_font_6x12_tf);
  snprintf(b, sizeof(b), "%u/%u", app_meals_24h(), MEALS_PER_24H);
  _oled.drawUTF8(128 - _oled.getUTF8Width(b), 10, b);
  _oled.drawUTF8(128 - _oled.getUTF8Width("24 h"), 22, "24 h");

  if (!clock_valid()) {
    _texto(44, "Sem hora: Wi-Fi ou");
    _texto(58, "o comando 'hora'");
    return;
  }
  uint32_t prox = app_next_meal_at();
  if (prox == 0) {
    _texto(44, "Automático desligado");
  } else {
    char h[8];
    _hhmm(h, sizeof(h), prox);
    snprintf(b, sizeof(b), "Próx %s · %u grãos", h, app_config().grains);
    _texto(44, b);
  }

  if (app_has_error()) {
    _texto(58, _erro);
    return;
  }
  uint32_t idade;
  if (!app_last_feed_age(idade)) {
    _texto(58, "Nunca alimentou");
  } else if (idade < 60) {
    _texto(58, "Última: agora");
  } else if (idade < 3600) {
    snprintf(b, sizeof(b), "Última: há %lu min", (unsigned long)(idade / 60));
    _texto(58, b);
  } else {
    snprintf(b, sizeof(b), "Última: há %lu h", (unsigned long)(idade / 3600));
    _texto(58, b);
  }
}

static void _pagina_24h() {
  char b[40];
  _oled.setFont(u8g2_font_6x12_tf);
  _texto(10, "Últimas 24 h");
  snprintf(b, sizeof(b), "Refeições: %u de %u", app_meals_24h(), MEALS_PER_24H);
  _texto(22, b);

  uint32_t idade;
  const History& h = app_history();
  if (!app_last_feed_age(idade)) {
    _texto(34, "Última: nenhuma");
  } else if (h.last_reason == (uint8_t)MealReason::SENSOR) {
    _texto(34, "Última: pelo servo");
  } else {
    snprintf(b, sizeof(b), "Última: %u/%u grãos", h.last_conf, h.last_req);
    _texto(34, b);
  }

  if (!doser_sensor_tested()) {
    _texto(46, "Sensor: não testado");
  } else {
    _texto(46, doser_sensor_ok() ? "Sensor: ok" : "Sensor: FALHOU");
  }
  _texto(58, app_calibration().valid ? "Doseador: calibrado" : "Doseador: CALIBRAR");
}

static void _pagina_conexoes() {
  char b[40];
  _oled.setFont(u8g2_font_6x12_tf);
  _texto(10, link_connected() ? "Aquário: no fio" : "Aquário: fora do fio");
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(b, sizeof(b), "Wi-Fi: %d dBm", (int)WiFi.RSSI());
    _texto(22, b);
    _texto(34, WiFi.localIP().toString().c_str());
  } else {
    _texto(22, "Wi-Fi: desconectado");
  }
  if (!clock_rtc_present()) {
    _texto(46, "Relógio: sem DS3231");
  } else {
    _texto(46, clock_ntp_synced() ? "Relógio: ok + NTP" : "Relógio: ok");
  }
  snprintf(b, sizeof(b), "fw %s", FW_VERSION);
  _texto(58, b);
}

static void _tela_programa() {
  static const char* NOMES[4] = {"1ª refeição", "2ª refeição", "Grãos", "Automático"};
  char b[24];
  _oled.setFont(u8g2_font_6x12_tf);
  snprintf(b, sizeof(b), "Programação %u/4", _campo + 1);
  _texto(10, b);
  _texto(24, NOMES[_campo]);
  switch (_campo) {
    case 0: snprintf(b, sizeof(b), "%02u h", _edicao.hour1); break;
    case 1: snprintf(b, sizeof(b), "%02u h", _edicao.hour2); break;
    case 2: snprintf(b, sizeof(b), "%u grãos", _edicao.grains); break;
    default: snprintf(b, sizeof(b), "%s", _edicao.auto_enabled ? "ligado" : "desligado"); break;
  }
  _oled.setFont(u8g2_font_10x20_tf);
  _texto(43, b);
  _oled.setFont(u8g2_font_5x8_tf);
  _texto(53, "1× +1  3× -1  2× próximo");
  _texto(62, "Segure 2 s: salva e sai");
}

static void _tela_calibra() {
  char b[24];
  _oled.setFont(u8g2_font_6x12_tf);
  _texto(10, "Calibração");
  snprintf(b, sizeof(b), "%s Repouso %3u°", _alvo == 0 ? ">" : " ", _cal_edicao.rest_deg);
  _texto(26, b);
  snprintf(b, sizeof(b), "%s Despejo %3u°", _alvo == 1 ? ">" : " ", _cal_edicao.dump_deg);
  _texto(40, b);
  _oled.setFont(u8g2_font_5x8_tf);
  _texto(53, "1× +2°  3× -2°  2× troca");
  _texto(62, "Segure 2 s: salva e sai");
}

static void _tela_atividade() {
  char b[24];
  _oled.setFont(u8g2_font_6x12_tf);
  switch (doser_phase()) {
    case DoserPhase::WARNING_TEST:
      _centro(26, "Aviso ao peixe");
      _centro(42, "teste, sem ração");
      return;
    case DoserPhase::BEAM_TEST:
      _centro(20, "Teste do feixe");
      snprintf(b, sizeof(b), "%u passaram", doser_beam_count());
      _centro(40, b);
      return;
    case DoserPhase::WARNING:   _centro(14, "Alimentando"); _centro(30, "avisando o peixe"); break;
    case DoserPhase::SELFTEST:  _centro(14, "Alimentando"); _centro(30, "testando o sensor"); break;
    case DoserPhase::FINISHING: _centro(14, "Alimentando"); _centro(30, "terminando"); break;
    default:                    _centro(14, "Alimentando"); _centro(30, "dosando"); break;
  }
  uint8_t req = doser_requested();
  if (req == 0) return;
  if (!doser_sensor_ok()) {
    _centro(46, "contando pelo servo");
    return;
  }
  uint8_t conf = doser_confirmed();
  snprintf(b, sizeof(b), "%u de %u grãos", conf, req);
  _centro(46, b);
  _oled.drawFrame(14, 53, 100, 8);
  uint8_t w = (uint8_t)((uint16_t)(conf > req ? req : conf) * 98 / req);
  if (w) _oled.drawBox(15, 54, w, 6);
}

static void _desenha() {
  _oled.clearBuffer();
  if (_msg_ativa) {
    _oled.setFont(u8g2_font_6x12_tf);
    _centro(28, _msg1);
    _centro(44, _msg2);
  } else if (_modo == Modo::PROGRAMA) {
    _tela_programa();
  } else if (_modo == Modo::CALIBRA) {
    _tela_calibra();
  } else if (_em_atividade()) {
    _tela_atividade();
  } else {
    switch (_pagina) {
      case 0:  _pagina_estado(); break;
      case 1:  _pagina_24h(); break;
      default: _pagina_conexoes(); break;
    }
  }
  _oled.sendBuffer();
}

void ui_update() {
  button_poll(_gestos);
  uint32_t agora = millis();
  _gesto(_gestos.update(agora));

  // Parado demais na programação ou na calibração: cancela, nada muda.
  if (_modo == Modo::PROGRAMA && agora - _ultimo_toque >= PROGRAM_IDLE_MS) {
    _modo = Modo::NORMAL;
    _mensagem("Edição cancelada", "nada mudou", 3000);
  }
  if (_modo == Modo::CALIBRA && agora - _ultimo_toque >= CALIBRATE_IDLE_MS) _sair_calibracao(false);

  if (_em_atividade()) _acender(2000);
  if (_msg_ativa && (int32_t)(agora - _msg_ate) >= 0) {
    _msg_ativa = false;
    _redesenhar = true;
  }

  if (_tela && _modo == Modo::NORMAL && (int32_t)(agora - _acesa_ate) >= 0) {
    _apagar();
  } else if (_tela && _tem_tela && (_redesenhar || agora - _ultimo_desenho >= REDRAW_MS)) {
    _redesenhar = false;
    _ultimo_desenho = agora;
    _desenha();   // o quadro novo vai antes de o painel acender: nada de quadro velho
    if (!_ligada) {
      _oled.setPowerSave(0);
      _ligada = true;
    }
  }

  // LED da placa: aceso na refeição; piscando (1 Hz) com aviso pendente.
  bool led = _em_refeicao() || (app_has_error() && (agora / 500) % 2 == 0);
  hw_status_led(led);
}

void ui_meal_result(const MealResult& r, bool test) {
  char b[32];
  snprintf(b, sizeof(b), "%u de %u grãos", r.confirmed, r.requested);
  if (test) {
    if (r.reason == MealReason::SENSOR) {
      _mensagem("Teste: sensor falhou", "o grão não foi visto", 8000);
    } else {
      _mensagem(r.ok ? "Teste: grão visto" : "Teste: nada caiu", b, 8000);
    }
    return;
  }
  switch (r.reason) {
    case MealReason::VAZIO:
      _mensagem("Sem ração? Travou?", b, 8000);
      snprintf(_erro, sizeof(_erro), "! Faltou ração: %u/%u", r.confirmed, r.requested);
      break;
    case MealReason::SENSOR:
      _mensagem("Sensor falhou", "contou pelo servo", 8000);
      _copia(_erro, sizeof(_erro), "! Sensor falhou");
      break;
    case MealReason::OK:
      _mensagem(r.ok ? "Alimentado" : "Incompleta", b, 5000);
      break;
  }
}

void ui_denied(DenyReason why, Origin origin) {
  bool agenda = origin == Origin::AGENDA || origin == Origin::RECUP;
  switch (why) {
    case DenyReason::LIMITE: {
      char b[32];
      snprintf(b, sizeof(b), "%u refeições em 24 h", MEALS_PER_24H);
      _mensagem("Limite atingido", b, 5000);
      if (agenda) _copia(_erro, sizeof(_erro), "! Agenda: limite 24 h");
      break;
    }
    case DenyReason::OCUPADO:
      _mensagem("Ocupado", "tente de novo", 3000);
      break;
    case DenyReason::CALIBRAR:
      _mensagem("Doseador sem", "calibração", 5000);
      if (agenda) _copia(_erro, sizeof(_erro), "! Agenda: calibrar");
      break;
    case DenyReason::PERDIDA:
      _mensagem("Refeição perdida", "não recuperada", 8000);
      _copia(_erro, sizeof(_erro), "! Refeição perdida");
      break;
  }
}

bool ui_begin_calibration() {
  if (_modo == Modo::CALIBRA) return true;
  if (doser_busy() || ota_in_progress()) return false;
  _cal_edicao = app_calibration();
  _alvo = 0;
  if (!doser_servo_hold(_cal_edicao.rest_deg)) return false;
  _modo = Modo::CALIBRA;
  _msg_ativa = false;
  _ultimo_toque = millis();
  _acender();
  return true;
}

bool ui_calibrating() { return _modo == Modo::CALIBRA; }

bool ui_has_display() { return _tem_tela; }
