#include "app.h"

#include <Arduino.h>

#include "clock.h"
#include "debuglog.h"
#include "doser.h"
#include "link.h"
#include "ota_manager.h"
#include "schedule.h"
#include "store.h"
#include "ui.h"

static FeederConfig _cfg;
static Calibration  _cal;
static History      _hist;
/** Primeiro boot, ou agenda mudada sem hora: dar a agenda atual por tratada quando houver hora. */
static bool         _marcar_agenda = false;

// Refeições desde o boot pelo millis(): o limite de 24 h segue valendo mesmo
// sem hora válida (bateria do DS3231 acabou, sem NTP).
static const uint8_t RAM_MEALS = MEALS_PER_24H + 3;
static uint32_t _meals_ms[RAM_MEALS] = {};
static uint8_t  _meals_ms_next = 0;
static bool     _alimentou_desde_boot = false;
static uint32_t _ultima_ms = 0;

static uint32_t _m1_ms = 0;            // último pulso do M1, pelo millis()
static uint32_t _tick_agenda = 0;
static uint32_t _tick_m1 = 0;
static bool     _erro = false;
static bool     _teste_grao = false;   // a dose em andamento é o `teste grao` do console

/** Algo físico em andamento, ou prestes a reiniciar: nada novo começa. */
static bool _ocupado() { return doser_busy() || ui_calibrating() || ota_in_progress(); }

static uint8_t _meals_desde_boot() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < RAM_MEALS; i++) {
    if (_meals_ms[i] != 0 && millis() - _meals_ms[i] < 86400000UL) n++;
  }
  return n;
}

uint8_t app_meals_24h() {
  uint8_t por_relogio = clock_valid() ? meals_in_24h(_hist, clock_now()) : 0;
  uint8_t por_millis = _meals_desde_boot();
  // As duas contam as mesmas refeições desde o boot; a maior vale.
  return por_relogio > por_millis ? por_relogio : por_millis;
}

static void _deny(DenyReason why, Origin origin) {
  char b[48];
  if (link_format_denied(b, sizeof(b), why, app_meals_24h(), origin)) link_notify(b);
  // Botão e app: quem pediu está vendo a recusa. Da agenda, ninguém está.
  if (origin == Origin::AGENDA || origin == Origin::RECUP) _erro = true;
  ui_denied(why, origin);
  Log.printf("[Refeicao] Recusada: %s (%s)\n", deny_token(why), origin_token(origin));
}

static void _start_meal(uint8_t grains, Origin origin) {
  uint32_t agora = clock_valid() ? clock_now() : 0;
  if (agora) {
    history_add_meal(_hist, agora);
    // O M1 roda em toda refeição: conta como o pulso do dia.
    _hist.last_m1_at = agora;
  }
  _meals_ms[_meals_ms_next] = millis() ? millis() : 1;
  _meals_ms_next = (uint8_t)((_meals_ms_next + 1) % RAM_MEALS);
  _m1_ms = millis();
  // Gravado ANTES de começar: se a energia cair no meio, a refeição já contou
  // para o limite e o horário já está dado por servido. Na dúvida, menos ração.
  store_save_history(_hist);
  _teste_grao = false;
  doser_begin_meal(grains ? grains : _cfg.grains, origin, _cal);
  Log.printf("[Refeicao] %u graos (%s)\n", grains ? grains : _cfg.grains, origin_token(origin));
}

void app_feed(uint8_t grains, Origin origin) {
  if (_ocupado()) return _deny(DenyReason::OCUPADO, origin);
  if (!_cal.valid) return _deny(DenyReason::CALIBRAR, origin);
  if (origin != Origin::FORCADO && app_meals_24h() >= MEALS_PER_24H) {
    return _deny(DenyReason::LIMITE, origin);
  }
  _start_meal(grains, origin);
}

bool app_test_warning() {
  if (_ocupado()) return false;
  doser_begin_warning_test();
  return true;
}

bool app_test_grain() {
  if (_ocupado() || !_cal.valid) return false;
  _teste_grao = true;
  doser_begin_meal(1, Origin::BOTAO, _cal);
  return true;
}

bool app_test_beam(uint16_t seconds) {
  if (_ocupado()) return false;
  doser_begin_beam_test(seconds);
  return true;
}

bool app_test_m1() {
  if (_ocupado()) return false;
  doser_begin_m1_pulse(M1_DAILY_MS);
  return true;
}

bool app_move_servo(uint8_t deg) {
  if (_ocupado()) return false;
  return doser_servo_park(deg);
}

static void _agenda() {
  // Ocupado (pulso do M1, aviso de teste, calibração, OTA): espera, sem perder a vez.
  if (!clock_valid() || _ocupado()) return;
  uint32_t agora = clock_now();
  if (_marcar_agenda) {
    schedule_mark_current(_cfg, _hist, agora);
    store_save_history(_hist);
    _marcar_agenda = false;
    return;
  }
  for (uint8_t volta = 0; volta < 2; volta++) {
    SlotDecision d = schedule_decide(_cfg, _hist, agora);
    if (d.action == SlotAction::NONE) return;
    _hist.served[d.slot] = d.occurrence;
    if (d.action == SlotAction::MISS) {
      store_save_history(_hist);
      _deny(DenyReason::PERDIDA, Origin::AGENDA);
      continue;   // pode haver a outra pendente
    }
    Origin origem = d.action == SlotAction::RECOVER ? Origin::RECUP : Origin::AGENDA;
    if (!_cal.valid) {
      store_save_history(_hist);
      return _deny(DenyReason::CALIBRAR, origem);
    }
    if (app_meals_24h() >= MEALS_PER_24H) {
      store_save_history(_hist);
      return _deny(DenyReason::LIMITE, origem);
    }
    return _start_meal(0, origem);
  }
}

/**
 * M1 pelo menos uma vez por dia, com refeição ou sem (agenda desligada,
 * limite, módulo recém-ligado): ração parada empelota.
 */
static void _m1_diario() {
  if (_ocupado()) return;
  bool vencido;
  if (clock_valid()) {
    uint32_t agora = clock_now();
    if (_hist.last_m1_at > agora) _hist.last_m1_at = agora;   // relógio voltou: recomeça a contar
    vencido = _hist.last_m1_at == 0 || agora - _hist.last_m1_at >= M1_DAILY_INTERVAL_S;
  } else {
    vencido = millis() - _m1_ms >= M1_DAILY_INTERVAL_S * 1000UL;
  }
  if (!vencido) return;
  doser_begin_m1_pulse(M1_DAILY_MS);
  _m1_ms = millis();
  if (clock_valid()) {
    _hist.last_m1_at = clock_now();
    store_save_history(_hist);
  }
  Log.println("[M1] Pulso diario");
}

static void _resultado() {
  MealResult r;
  if (!doser_take_result(r)) return;
  if (_teste_grao) {
    // Bancada: não é refeição — fora do histórico, do limite e do principal.
    _teste_grao = false;
    ui_meal_result(r, true);
    Log.printf("[Teste] Grao: %u de 1, %s\n", r.confirmed, reason_token(r.reason));
    return;
  }
  if (clock_valid()) _hist.last_feed_at = clock_now();
  _hist.last_req = r.requested;
  _hist.last_conf = r.confirmed;
  _hist.last_ok = r.ok;
  _hist.last_reason = (uint8_t)r.reason;
  _alimentou_desde_boot = true;
  _ultima_ms = millis();
  store_save_history(_hist);

  char b[48];
  if (link_format_fed(b, sizeof(b), r)) link_notify(b);
  if (r.reason != MealReason::OK) _erro = true;
  ui_meal_result(r, false);
  Log.printf("[Refeicao] Fim: %u de %u, %s\n", r.confirmed, r.requested, reason_token(r.reason));
}

static void _enlace() {
  LinkRequest req;
  while (link_poll(req)) {
    if (req.cmd == LinkCmd::PING) {
      if (link_take_reconnected()) {
        char s[40];
        if (link_format_schedule(s, sizeof(s), _cfg)) link_send(s);
        link_flush_pending();
      }
      uint32_t idade = UINT32_MAX;
      if (!app_last_feed_age(idade)) idade = UINT32_MAX;
      char b[64];
      if (link_format_pong(b, sizeof(b), _cfg, idade, _hist.last_req, _hist.last_conf, _hist.last_ok,
                           app_meals_24h())) {
        link_send(b);
      }
    } else if (req.cmd == LinkCmd::FEED) {
      app_feed(req.grains, req.force ? Origin::FORCADO : Origin::APP);
    } else if (req.cmd == LinkCmd::CFG) {
      app_save_config(req.cfg);
    }
  }
}

void app_init() {
  _cfg = store_load_config();
  _cal = store_load_calibration();
  doser_set_calibration(_cal);
  _marcar_agenda = !store_load_history(_hist);
  _m1_ms = millis();
  _tick_m1 = millis();
}

void app_update() {
  // A atualização termina num reinício: refeição, teste ou calibração em
  // andamento param agora, em estado seguro, e não no meio do reinício.
  if (ota_in_progress() && doser_busy()) {
    doser_abort();
    Log.println("[OTA] Doseador interrompido para a atualizacao");
  }
  _enlace();
  _resultado();
  uint32_t agora = millis();
  if (agora - _tick_agenda >= 1000) {
    _tick_agenda = agora;
    _agenda();
  }
  if (agora - _tick_m1 >= 60000) {
    _tick_m1 = agora;
    _m1_diario();
  }
}

bool app_save_config(const FeederConfig& cfg) {
  if (cfg.hour1 > 23 || cfg.hour2 > 23 || cfg.grains < GRAINS_MIN || cfg.grains > GRAINS_MAX) return false;
  bool mudou = cfg.hour1 != _cfg.hour1 || cfg.hour2 != _cfg.hour2 || cfg.grains != _cfg.grains ||
               cfg.auto_enabled != _cfg.auto_enabled;
  // A mesma agenda de novo (o app reenviando) não gasta a flash nem mexe na
  // refeição pendente; só responde com o SCHEDULE.
  if (mudou) {
    FeederConfig antes = _cfg;
    _cfg = cfg;
    store_save_config(_cfg);
    // Vale da próxima ocorrência em diante: mudar o horário para "agora" não
    // alimenta na hora.
    if (clock_valid()) {
      schedule_apply_change(antes, _cfg, _hist, clock_now());
      store_save_history(_hist);
    } else {
      _marcar_agenda = true;   // sem hora: a agenda inteira vale da próxima, quando a hora voltar
    }
  }
  char s[40];
  if (link_format_schedule(s, sizeof(s), _cfg)) link_notify(s);
  Log.printf("[Agenda] %02uh e %02uh, %u graos, automatico %s\n", _cfg.hour1, _cfg.hour2, _cfg.grains,
                _cfg.auto_enabled ? "ligado" : "desligado");
  return true;
}

void app_save_calibration(const Calibration& cal) {
  _cal = cal;
  _cal.valid = true;
  store_save_calibration(_cal);
  doser_set_calibration(_cal);
  Log.printf("[Calibracao] Repouso %u, despejo %u\n", _cal.rest_deg, _cal.dump_deg);
}

void app_factory_reset() {
  doser_abort();
  store_factory_reset();
  Log.println("[NVS] Padrao de fabrica; reiniciando");
  delay(200);
  ESP.restart();
}

const FeederConfig& app_config() { return _cfg; }
const Calibration& app_calibration() { return _cal; }
const History& app_history() { return _hist; }
bool app_has_error() { return _erro; }
void app_clear_error() { _erro = false; }

uint32_t app_next_meal_at() {
  if (!_cfg.auto_enabled || !clock_valid()) return 0;
  uint32_t agora = clock_now();
  return agora + seconds_to_next_slot(_cfg, agora);
}

bool app_last_feed_age(uint32_t& age_s) {
  if (_alimentou_desde_boot) {
    age_s = (millis() - _ultima_ms) / 1000UL;
    return true;
  }
  if (_hist.last_feed_at != 0 && clock_valid() && clock_now() >= _hist.last_feed_at) {
    age_s = clock_now() - _hist.last_feed_at;
    return true;
  }
  return false;
}
