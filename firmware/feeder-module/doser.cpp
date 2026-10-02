#include "doser.h"

#include "hw.h"

enum class Step : uint8_t {
  IDLE,
  WARN_UP, WARN_HOLD, WARN_DOWN,   // aviso ao peixe
  TEST_DARK, TEST_LIT,             // autoteste do sensor
  M1_PRE,                          // solta a ração antes do primeiro grão
  DUMP_MOVE, DUMP_WAIT,            // bolso sobre o tubo; grão caindo pelo feixe
  BACK_MOVE, BACK_WAIT,            // bolso de volta sob o reservatório; enchendo
  M1_RETRY,                        // movimento vazio: vibra antes de repetir
  END_MOVE, END_WAIT,              // slide em repouso, servo solto
  PULSE,                           // pulso diário do M1
  BEAM,                            // teste do feixe: conta o que passar
};

static Step     _step = Step::IDLE;
static uint32_t _step_at = 0;
static uint32_t _meal_at = 0;
static bool     _warning_only = false;
static uint16_t _pulse_ms = 0;
static bool     _beam_test = false;
static uint32_t _beam_ms = 0;
static uint16_t _beam_count = 0;

static Origin      _origin = Origin::AGENDA;
static Calibration _cal = {SERVO_DEFAULT_REST, SERVO_DEFAULT_DUMP, false};
static uint8_t     _req = 0;
static uint8_t     _conf = 0;
static uint8_t     _dumps = 0;
static uint8_t     _empty = 0;
static bool        _sensor_ok = true;
static bool        _sensor_tested = false;
static int         _dark_level = 0;
static MealReason  _reason = MealReason::OK;

static bool       _has_result = false;
static MealResult _result = {};

// Servo com velocidade limitada. A posição é a última escrita: o servo não
// informa onde está, então todo movimento passa por aqui.
static bool     _servo_on = false;
static bool     _pos_known = false;   // depois do boot, o slide está no repouso
static uint8_t  _servo_deg = 0;
static uint8_t  _servo_target = 0;
static uint32_t _servo_step_at = 0;
// Movimento manual (calibração, console), fora das refeições.
static bool     _park = false;        // soltar o servo quando chegar
static uint32_t _park_at = 0;

// Teto absoluto dos motores, à parte das fases: um engano na máquina de
// estados não pode deixar motor ligado.
static bool     _m1_on = false;
static bool     _m2_on = false;
static uint32_t _m1_since = 0;
static uint32_t _m2_since = 0;

static void _go(Step s) {
  _step = s;
  _step_at = hw_millis();
}

static void _m1_set(bool on) {
  if (on && !_m1_on) _m1_since = hw_millis();
  _m1_on = on;
  hw_m1_duty(on ? M1_DUTY_PCT : 0);
}

static void _m2_set(uint8_t pct) {
  if (pct > 0 && !_m2_on) _m2_since = hw_millis();
  _m2_on = pct > 0;
  hw_m2_duty(pct);
}

static void _servo_go(uint8_t deg) {
  if (!_servo_on) {
    if (!_pos_known) {
      _servo_deg = _cal.rest_deg;
      _pos_known = true;
    }
    hw_servo_attach();
    _servo_on = true;
    hw_servo_write_deg(_servo_deg);
  }
  _servo_target = deg;
}

static bool _servo_arrived() { return _servo_deg == _servo_target; }

static void _servo_tick(uint32_t now) {
  if (!_servo_on || _servo_deg == _servo_target) return;
  if (now - _servo_step_at < SERVO_STEP_MS) return;
  _servo_step_at = now;
  if (_servo_deg < _servo_target) {
    uint16_t prox = (uint16_t)_servo_deg + SERVO_STEP_DEG;
    _servo_deg = prox > _servo_target ? _servo_target : (uint8_t)prox;
  } else {
    int16_t prox = (int16_t)_servo_deg - SERVO_STEP_DEG;
    _servo_deg = prox < (int16_t)_servo_target ? _servo_target : (uint8_t)prox;
  }
  hw_servo_write_deg(_servo_deg);
}

static void _servo_release() {
  if (!_servo_on) return;
  hw_servo_detach();
  _servo_on = false;
}

/** Cinco leituras iguais: nível firme. Diferentes: o LM393 oscila. */
static bool _sample(int& level) {
  int a = hw_ir_level();
  for (uint8_t i = 0; i < 4; i++) {
    if (hw_ir_level() != a) return false;
  }
  level = a;
  return true;
}

static void _publish() {
  _result.requested = _req;
  _result.confirmed = _sensor_ok ? _conf : 0;
  _result.ok = _sensor_ok && _conf >= _req;
  _result.reason = _reason;
  _result.origin = _origin;
  _has_result = true;
}

static void _finish() {
  hw_ir_disarm();
  hw_ir_led(false);
  _m1_set(false);
  _m2_set(0);
  if (_servo_on) {
    _servo_go(_cal.rest_deg);
    _go(Step::END_MOVE);
  } else {
    _publish();
    _go(Step::IDLE);
  }
}

static void _dump() {
  hw_ir_take_count();   // zera: conta só o que cair neste movimento
  _dumps++;
  _servo_go(_cal.dump_deg);
  _go(Step::DUMP_MOVE);
}

/** Depois de o slide voltar: mais um grão, uma nova tentativa, ou fim. */
static void _next() {
  if (_sensor_ok) {
    if (_conf >= _req) return _finish();
    if (_empty >= EMPTY_TRIES) {
      _reason = MealReason::VAZIO;
      return _finish();
    }
    if (_dumps >= (uint16_t)_req * DUMPS_PER_GRAIN + 3) return _finish();
    if (_empty > 0) {
      _m1_set(true);
      _go(Step::M1_RETRY);
      return;
    }
    return _dump();
  }
  // Sem sensor: um movimento por grão pedido, sem repetir — cada um leva no
  // máximo o que cabe no bolso, então o teto é o próprio número de grãos.
  if (_dumps >= _req) return _finish();
  _dump();
}

void doser_set_calibration(const Calibration& cal) { _cal = cal; }

void doser_begin_meal(uint8_t grains, Origin origin, const Calibration& cal) {
  if (_step != Step::IDLE) return;
  if (grains < GRAINS_MIN) grains = GRAINS_MIN;
  if (grains > GRAINS_MAX) grains = GRAINS_MAX;
  _req = grains;
  _conf = 0;
  _dumps = 0;
  _empty = 0;
  _origin = origin;
  _cal = cal;
  _warning_only = false;
  _park = false;
  _sensor_ok = true;
  _reason = MealReason::OK;
  _has_result = false;
  _meal_at = hw_millis();
  _go(Step::WARN_UP);
}

bool doser_servo_hold(uint8_t deg) {
  if (_step != Step::IDLE) return false;
  _park = false;
  _servo_go(deg > 180 ? 180 : deg);
  return true;
}

bool doser_servo_park(uint8_t deg) {
  if (!doser_servo_hold(deg)) return false;
  _park = true;
  _park_at = hw_millis();
  return true;
}

void doser_begin_warning_test() {
  if (_step != Step::IDLE) return;
  _warning_only = true;
  _meal_at = hw_millis();
  _go(Step::WARN_UP);
}

void doser_begin_m1_pulse(uint16_t ms) {
  if (_step != Step::IDLE) return;
  _pulse_ms = ms > M1_MAX_MS ? M1_MAX_MS : ms;
  _m1_set(true);
  _go(Step::PULSE);
}

void doser_begin_beam_test(uint16_t seconds) {
  if (_step != Step::IDLE) return;
  _beam_test = true;
  _beam_ms = (uint32_t)(seconds > BEAM_TEST_MAX_S ? BEAM_TEST_MAX_S : seconds) * 1000UL;
  _beam_count = 0;
  hw_ir_led(false);
  _go(Step::TEST_DARK);
}

void doser_update() {
  uint32_t now = hw_millis();
  if (_m1_on && now - _m1_since > M1_MAX_MS) _m1_set(false);
  if (_m2_on && now - _m2_since > M2_MAX_MS) _m2_set(0);
  _servo_tick(now);

  if (_step == Step::IDLE) {
    if (_park && _servo_on) {
      if (!_servo_arrived()) {
        _park_at = now;
      } else if (now - _park_at >= SERVO_SETTLE_MS) {
        _servo_release();
        _park = false;
      }
    }
    return;
  }
  uint32_t t = now - _step_at;

  bool refeicao = !_warning_only && !_beam_test && _step != Step::PULSE;
  if (refeicao && _step != Step::END_MOVE && _step != Step::END_WAIT && now - _meal_at > MEAL_MAX_MS) {
    _finish();
    return;
  }

  switch (_step) {
    case Step::WARN_UP: {
      uint32_t r = t > M2_RAMP_MS ? M2_RAMP_MS : t;
      _m2_set((uint8_t)((uint32_t)M2_DUTY_PCT * r / M2_RAMP_MS));
      if (t >= M2_RAMP_MS) _go(Step::WARN_HOLD);
      break;
    }
    case Step::WARN_HOLD:
      _m2_set(M2_DUTY_PCT);
      if (t >= M2_HOLD_MS) _go(Step::WARN_DOWN);
      break;
    case Step::WARN_DOWN: {
      uint32_t r = t > M2_RAMP_MS ? M2_RAMP_MS : t;
      _m2_set((uint8_t)((uint32_t)M2_DUTY_PCT * (M2_RAMP_MS - r) / M2_RAMP_MS));
      if (t < M2_RAMP_MS) break;
      _m2_set(0);
      if (_warning_only) {
        _warning_only = false;
        _go(Step::IDLE);
      } else {
        hw_ir_led(false);
        _go(Step::TEST_DARK);
      }
      break;
    }
    case Step::TEST_DARK: {
      if (t < SELFTEST_SETTLE_MS) break;
      int nivel = 0;
      _sensor_ok = _sample(nivel);
      _dark_level = nivel;
      hw_ir_led(true);
      _go(Step::TEST_LIT);
      break;
    }
    case Step::TEST_LIT: {
      if (t < SELFTEST_SETTLE_MS) break;
      int nivel = 0;
      bool firme = _sample(nivel);
      // Apagado tem de ler "bloqueado" e aceso "livre": iguais é LED
      // queimado, feixe desalinhado, grão preso ou luz entrando.
      _sensor_ok = _sensor_ok && firme && nivel != _dark_level;
      _sensor_tested = true;
      if (_beam_test) {
        if (_sensor_ok && _beam_ms > 0) {
          hw_ir_arm(_dark_level);
          _go(Step::BEAM);
        } else {
          hw_ir_led(false);
          _beam_test = false;
          _go(Step::IDLE);
        }
        break;
      }
      if (_sensor_ok) {
        hw_ir_arm(_dark_level);
      } else {
        hw_ir_led(false);
        _reason = MealReason::SENSOR;
      }
      _m1_set(true);
      _go(Step::M1_PRE);
      break;
    }
    case Step::M1_PRE:
      if (t >= M1_PRE_MS) {
        _m1_set(false);
        _dump();
      }
      break;
    case Step::DUMP_MOVE:
      if (_servo_arrived()) _go(Step::DUMP_WAIT);
      break;
    case Step::DUMP_WAIT:
      if (t < (_sensor_ok ? DETECT_WINDOW_MS : SERVO_SETTLE_MS)) break;
      if (_sensor_ok) {
        uint16_t vistos = hw_ir_take_count();
        if (vistos > 0) {
          uint16_t total = (uint16_t)_conf + vistos;
          _conf = total > GRAINS_MAX ? GRAINS_MAX : (uint8_t)total;
          _empty = 0;
        } else {
          _empty++;
        }
      }
      _servo_go(_cal.rest_deg);
      _go(Step::BACK_MOVE);
      break;
    case Step::BACK_MOVE:
      if (_servo_arrived()) _go(Step::BACK_WAIT);
      break;
    case Step::BACK_WAIT:
      if (t >= SERVO_SETTLE_MS) _next();
      break;
    case Step::M1_RETRY:
      if (t >= M1_RETRY_MS) {
        _m1_set(false);
        _dump();
      }
      break;
    case Step::END_MOVE:
      if (_servo_arrived()) _go(Step::END_WAIT);
      break;
    case Step::END_WAIT:
      if (t >= SERVO_SETTLE_MS) {
        _servo_release();
        _publish();
        _go(Step::IDLE);
      }
      break;
    case Step::PULSE:
      if (t >= _pulse_ms) {
        _m1_set(false);
        _go(Step::IDLE);
      }
      break;
    case Step::BEAM: {
      uint32_t total = (uint32_t)_beam_count + hw_ir_take_count();
      _beam_count = total > 0xFFFF ? 0xFFFF : (uint16_t)total;
      if (t >= _beam_ms) {
        hw_ir_disarm();
        hw_ir_led(false);
        _beam_test = false;
        _go(Step::IDLE);
      }
      break;
    }
    case Step::IDLE:
      break;
  }
}

void doser_abort() {
  hw_ir_disarm();
  hw_ir_led(false);
  _m1_set(false);
  _m2_set(0);
  _servo_release();
  _warning_only = false;
  _beam_test = false;
  _park = false;
  _go(Step::IDLE);
}

bool doser_busy() { return _step != Step::IDLE; }

DoserPhase doser_phase() {
  switch (_step) {
    case Step::IDLE:      return DoserPhase::IDLE;
    case Step::WARN_UP:
    case Step::WARN_HOLD:
    case Step::WARN_DOWN: return _warning_only ? DoserPhase::WARNING_TEST : DoserPhase::WARNING;
    case Step::TEST_DARK:
    case Step::TEST_LIT:  return _beam_test ? DoserPhase::BEAM_TEST : DoserPhase::SELFTEST;
    case Step::BEAM:      return DoserPhase::BEAM_TEST;
    case Step::END_MOVE:
    case Step::END_WAIT:  return DoserPhase::FINISHING;
    case Step::PULSE:     return DoserPhase::M1_PULSE;
    default:              return DoserPhase::DOSING;
  }
}

uint8_t doser_requested() { return _req; }
uint8_t doser_confirmed() { return _conf; }
bool doser_sensor_ok() { return _sensor_ok; }
bool doser_sensor_tested() { return _sensor_tested; }
uint16_t doser_beam_count() { return _beam_count; }

bool doser_take_result(MealResult& out) {
  if (!_has_result) return false;
  out = _result;
  _has_result = false;
  return true;
}
