#include "fan.h"

#include <Arduino.h>
#include <esp_arduino_version.h>
#include <math.h>

#include "config.h"
#include "device_config.h"
#include "event_log.h"
#include "temperature.h"

// ── PWM (LEDC) ──────────────────────────────────────────────────────────────
static const int LEDC_CHANNEL    = 0;
static const int LEDC_FREQ_HZ    = 25000;   // padrão de fan 4 pinos
static const int LEDC_RESOLUTION = 8;       // 0–255

#if ESP_ARDUINO_VERSION_MAJOR >= 3
#define FAN_LEDC_TARGET PIN_FAN
#else
#define FAN_LEDC_TARGET LEDC_CHANNEL
#endif

// ── Potenciômetro ───────────────────────────────────────────────────────────
static const int POT_MIN_ADC      = 80;                  // 0–4095
static const int POT_MIN_EXIT_ADC = POT_MIN_ADC + 40;    // histerese de saída
static const int POT_CHANGE_ADC   = 60;                  // movimento intencional

// ── Estados ─────────────────────────────────────────────────────────────────
enum FanMode      { FAN_MODE_AUTO, FAN_MODE_MANUAL_OFF, FAN_MODE_MANUAL_SPEED };
enum FanAutoState { FAN_AUTO_IDLE, FAN_AUTO_RUNNING, FAN_AUTO_COOLDOWN };

static FanMode      _mode      = FAN_MODE_AUTO;
static FanAutoState _auto_st   = FAN_AUTO_IDLE;
static int          _speed_pct = 0;

static unsigned long _cooldown_start_ms = 0;
static const unsigned long COOLDOWN_MS = (unsigned long)FAN_COOLDOWN_MIN * 60UL * 1000UL;

static int _last_manual_pct = 50;

// Tacômetro
static volatile uint32_t _tach_pulses    = 0;
static uint32_t          _tach_sample_ms = 0;
static int               _rpm            = 0;
static unsigned long     _stall_since_ms = 0;
static bool              _stall_reported = false;
/** Tempo com PWM e sem rotação antes de acusar defeito. */
static const unsigned long STALL_MS = 5000;

// Escalonamento progressivo
static int           _escalation_floor_pct = 0;
static float         _escalation_ref_temp  = 0.0f;
static unsigned long _escalation_ref_ms    = 0;

// Calibração do pot
static bool _pot_seen_min     = false;
static bool _pot_calibrated   = false;
static int  _pot_adc_prev     = -1;
static int  _pot_filtered_adc = -1;
static bool _pot_min_latched  = false;

static bool _temp_failsafe_active = false;

// ── Helpers ─────────────────────────────────────────────────────────────────

static void _apply_speed(int pct) {
  pct = constrain(pct, 0, 100);
  _speed_pct = pct;
  ledcWrite(FAN_LEDC_TARGET, (int)((long)pct * 255L / 100L));
  if (pct == 0) {
    // Zera a vigilância do tacômetro: sem PWM, rotação zero é o esperado.
    _stall_since_ms = 0;
    _stall_reported = false;
  }
}

static void IRAM_ATTR _tach_isr() { _tach_pulses++; }

static int _auto_speed(float delta) {
  if (delta <= 1.0f) return FAN_SPEED_LOW;
  if (delta <= 2.0f) return FAN_SPEED_MED;
  if (delta <= 3.0f) return FAN_SPEED_HIGH;
  return FAN_SPEED_MAX;
}

static void _reset_pot_calibration() {
  _pot_seen_min     = false;
  _pot_calibrated   = false;
  _pot_adc_prev     = -1;
  _pot_filtered_adc = -1;
  _pot_min_latched  = false;
}

static void _enter_manual_off() {
  _mode = FAN_MODE_MANUAL_OFF;
  _reset_pot_calibration();
  _temp_failsafe_active = false;
  _apply_speed(0);
}

// ── API ─────────────────────────────────────────────────────────────────────

void fan_init() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!ledcAttach(PIN_FAN, LEDC_FREQ_HZ, LEDC_RESOLUTION)) {
    event_log(SEV_FATAL, COMP_FAN, "fan.pwm_failed",
              "Nao foi possivel configurar o PWM da ventoinha");
    return;
  }
#else
  ledcSetup(LEDC_CHANNEL, LEDC_FREQ_HZ, LEDC_RESOLUTION);
  ledcAttachPin(PIN_FAN, LEDC_CHANNEL);
#endif
  pinMode(PIN_FAN_TACH, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_FAN_TACH), _tach_isr, FALLING);
  _tach_sample_ms = millis();
  _apply_speed(0);
}

void fan_on_schedule_reset() {
  _mode    = FAN_MODE_AUTO;
  _auto_st = FAN_AUTO_IDLE;
  _reset_pot_calibration();
  _temp_failsafe_active = false;
  _escalation_floor_pct = 0;
  // Não mexe na velocidade agora: `fan_update()` decide no próximo ciclo.
}

void fan_set_speed(int percent) {
  percent = constrain(percent, 0, 100);
  if (percent == 0) {
    _enter_manual_off();
    event_log(SEV_INFO, COMP_FAN, "fan.off", "Ventoinha desligada por comando");
    return;
  }
  _last_manual_pct = percent;
  _mode = FAN_MODE_MANUAL_SPEED;
  _apply_speed(percent);
  event_log(SEV_INFO, COMP_FAN, "fan.on",
            "Velocidade fixada em %d%% por comando", percent);
}

void fan_set_mode_auto(bool automatico) {
  if (automatico) {
    _mode    = FAN_MODE_AUTO;
    _auto_st = FAN_AUTO_IDLE;
    _reset_pot_calibration();
    _escalation_floor_pct = 0;
    event_log(SEV_INFO, COMP_FAN, "fan.mode_auto",
              "Controle automatico restaurado por comando");
  } else {
    _mode = FAN_MODE_MANUAL_SPEED;
    _apply_speed(_last_manual_pct);
    event_log(SEV_INFO, COMP_FAN, "fan.mode_manual",
              "Modo manual por comando (%d%%)", _last_manual_pct);
  }
}

int  fan_get_speed_percent() { return _speed_pct; }
bool fan_is_on()             { return _speed_pct > 0; }
int  fan_get_rpm()           { return _rpm; }

FanModeReport fan_get_mode_report() {
  if (_temp_failsafe_active) return FAN_REPORT_FAILSAFE;
  switch (_mode) {
    case FAN_MODE_AUTO:         return FAN_REPORT_AUTO;
    case FAN_MODE_MANUAL_OFF:   return FAN_REPORT_MANUAL_OFF;
    case FAN_MODE_MANUAL_SPEED: return FAN_REPORT_MANUAL;
  }
  return FAN_REPORT_AUTO;
}

void fan_update() {
  // Cópia: a task de rede pode reescrever a configuração no meio do ciclo.
  const DeviceConfig cfg = device_config_snapshot();
  unsigned long agora = millis();

  // ── 0. Tacômetro, amostrado a cada 2 s ────────────────────────────────────
  if (agora - _tach_sample_ms >= 2000UL) {
    noInterrupts();
    uint32_t pulses = _tach_pulses;
    _tach_pulses = 0;
    interrupts();
    uint32_t elapsed = agora - _tach_sample_ms;
    _tach_sample_ms  = agora;
    // 2 pulsos por volta → rpm = pulsos * 30000 / ms
    _rpm = elapsed > 0 ? (int)((pulses * 30000UL) / elapsed) : 0;

    /**
     * A vigilância que o firmware antigo não tinha. PWM acima de zero com
     * rotação zerada não é ambíguo: cabo solto, rolamento travado ou fonte
     * caída. Cinco segundos de tolerância cobrem a partida da ventoinha, que
     * leva um instante para o tacômetro registrar.
     */
    if (_speed_pct > 0 && _rpm == 0) {
      if (_stall_since_ms == 0) {
        _stall_since_ms = agora;
      } else if (!_stall_reported && agora - _stall_since_ms >= STALL_MS) {
        _stall_reported = true;
        event_log(SEV_ERROR, COMP_FAN, "fan.tach_stalled",
                  "PWM em %d%% mas tacometro em zero ha %lus", _speed_pct,
                  (agora - _stall_since_ms) / 1000);
      }
    } else if (_speed_pct > 0 && _rpm > 0) {
      if (_stall_reported) {
        event_log(SEV_INFO, COMP_FAN, "fan.tach_ok",
                  "Ventoinha voltou a girar (%d rpm)", _rpm);
      }
      _stall_since_ms = 0;
      _stall_reported = false;
    }
  }

  // ── 1. Potenciômetro ──────────────────────────────────────────────────────
  int pot_raw = analogRead(PIN_POT);
  if (_pot_filtered_adc < 0) {
    _pot_filtered_adc = pot_raw;
  } else {
    // Filtro exponencial simples contra o ruído do ADC.
    _pot_filtered_adc = (_pot_filtered_adc * 3 + pot_raw) / 4;
  }

  int pot_adc = _pot_filtered_adc;
  if (!_pot_min_latched) {
    if (pot_adc <= POT_MIN_ADC) _pot_min_latched = true;
  } else if (pot_adc >= POT_MIN_EXIT_ADC) {
    _pot_min_latched = false;
  }
  bool pot_at_min = _pot_min_latched;

  if (!_pot_calibrated) {
    // Calibração: só passa a valer depois de o usuário levar ao mínimo e subir.
    if (pot_at_min) {
      _pot_seen_min = true;
    } else if (_pot_seen_min) {
      _pot_calibrated = true;
      _pot_adc_prev   = pot_adc;
    }
  } else {
    if (pot_at_min) {
      if (_mode != FAN_MODE_MANUAL_OFF) {
        _enter_manual_off();
        event_log(SEV_INFO, COMP_POT, "fan.off",
                  "Ventoinha desligada pelo potenciometro");
      }
    } else if (_pot_adc_prev < 0 || abs(pot_adc - _pot_adc_prev) > POT_CHANGE_ADC) {
      _pot_adc_prev = pot_adc;
      int novo = constrain((int)map(pot_adc, POT_MIN_ADC, 4095, 1, 100), 1, 100);
      _last_manual_pct = novo;
      _mode = FAN_MODE_MANUAL_SPEED;
      _apply_speed(novo);
      event_log(SEV_INFO, COMP_POT, "fan.on",
                "Velocidade ajustada para %d%% pelo potenciometro", novo);
    }
  }

  // ── 2. Modo manual: nada mais a decidir ───────────────────────────────────
  if (_mode != FAN_MODE_AUTO) return;

  // ── 3. AUTO: histerese por temperatura ────────────────────────────────────
  if (!temperature_is_fresh()) {
    int failsafe = constrain(FAN_FAILSAFE_SPEED, 0, 100);
    if (!_temp_failsafe_active || _speed_pct != failsafe) {
      _apply_speed(failsafe);
      _temp_failsafe_active = true;
      event_log(SEV_WARN, COMP_FAN, "fan.failsafe",
                "Temperatura indisponivel; mantendo %d%% por precaucao", failsafe);
    }
    return;
  }

  if (_temp_failsafe_active) {
    _temp_failsafe_active = false;
    event_log(SEV_INFO, COMP_FAN, "fan.failsafe_cleared",
              "Temperatura voltou; controle automatico retomado");
  }

  float temp = temperature_read();

  switch (_auto_st) {
    case FAN_AUTO_IDLE:
      if (temp > cfg.fan_trigger_c) {
        float delta = temp - cfg.fan_trigger_c;
        int speed = max(_auto_speed(delta), _escalation_floor_pct);
        _apply_speed(speed);
        _auto_st = FAN_AUTO_RUNNING;
        _escalation_ref_temp = temp;
        _escalation_ref_ms   = agora;
        event_log(SEV_INFO, COMP_FAN, "fan.on",
                  "%.1fC (delta %.1f) acima do gatilho: %d%%", temp, delta, speed);
      }
      break;

    case FAN_AUTO_RUNNING:
      if (temp < cfg.fan_off_c) {
        _cooldown_start_ms    = agora;
        _escalation_floor_pct = 0;
        _apply_speed(FAN_SPEED_LOW);
        _auto_st = FAN_AUTO_COOLDOWN;
        event_log(SEV_INFO, COMP_FAN, "fan.cooldown",
                  "%.1fC abaixo do limiar: cooldown de %d min a %d%%", temp,
                  FAN_COOLDOWN_MIN, FAN_SPEED_LOW);
      } else if (temp >= cfg.fan_trigger_c) {
        float delta = temp - cfg.fan_trigger_c;
        int speed = max(_auto_speed(delta), _escalation_floor_pct);
        if (speed != _speed_pct) _apply_speed(speed);

        // Escalonamento: sem queda térmica suficiente, sobe o piso um degrau.
        if (agora - _escalation_ref_ms >=
            (unsigned long)FAN_ESCALATION_INTERVAL_MIN * 60000UL) {
          float queda = _escalation_ref_temp - temp;
          if (queda < (float)FAN_ESCALATION_DROP_C &&
              _escalation_floor_pct < FAN_SPEED_MAX) {
            int proximo;
            if      (_speed_pct < FAN_SPEED_LOW)  proximo = FAN_SPEED_LOW;
            else if (_speed_pct < FAN_SPEED_MED)  proximo = FAN_SPEED_MED;
            else if (_speed_pct < FAN_SPEED_HIGH) proximo = FAN_SPEED_HIGH;
            else                                  proximo = FAN_SPEED_MAX;
            if (proximo > _escalation_floor_pct) {
              _escalation_floor_pct = proximo;
              event_log(SEV_WARN, COMP_FAN, "fan.escalated",
                        "Queda de apenas %.1fC em %d min: piso sobe para %d%%",
                        queda, FAN_ESCALATION_INTERVAL_MIN, _escalation_floor_pct);
            }
          }
          _escalation_ref_temp = temp;
          _escalation_ref_ms   = agora;
        }
      } else {
        // Banda de histérese: temperatura aceitável, mantém e reinicia a
        // referência do escalonamento.
        _escalation_ref_temp = temp;
        _escalation_ref_ms   = agora;
      }
      break;

    case FAN_AUTO_COOLDOWN:
      if (temp > cfg.fan_trigger_c) {
        float delta = temp - cfg.fan_trigger_c;
        int speed = max(_auto_speed(delta), _escalation_floor_pct);
        _apply_speed(speed);
        _auto_st = FAN_AUTO_RUNNING;
        _escalation_ref_temp = temp;
        _escalation_ref_ms   = agora;
        event_log(SEV_INFO, COMP_FAN, "fan.on",
                  "Temperatura voltou a subir (%.1fC): %d%%", temp, speed);
      } else if (agora - _cooldown_start_ms >= COOLDOWN_MS) {
        _apply_speed(0);
        _auto_st = FAN_AUTO_IDLE;
        event_log(SEV_INFO, COMP_FAN, "fan.off", "Cooldown encerrado");
      }
      break;
  }
}
