#include "fan.h"
#include "config.h"
#include "temperature.h"
#include "log_manager.h"
#include <Arduino.h>
#include <esp_arduino_version.h>
#include <Preferences.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Configuração do PWM (LEDC)
// ---------------------------------------------------------------------------
static const int LEDC_CHANNEL    = 0;
static const int LEDC_FREQ_HZ    = 25000;   // 25 kHz — padrão PWM para fan 4 pinos (CPU fan)
static const int LEDC_RESOLUTION = 8;       // 8 bits → valores 0–255

#define FAN_CFG_NS             "aq_cfg"
#define FAN_CFG_KEY_TRIGGER    "fan_trig"
#define FAN_CFG_KEY_OFF        "fan_off"
static const float FAN_TRIGGER_MIN_C = 15.0f;
static const float FAN_TRIGGER_MAX_C = 45.0f;
static const float FAN_OFF_MIN_C     = 10.0f;
static const float FAN_OFF_MAX_C     = 44.5f;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
#define FAN_LEDC_TARGET PIN_FAN
#else
#define FAN_LEDC_TARGET LEDC_CHANNEL
#endif

#ifndef FAN_FAILSAFE_SPEED
#define FAN_FAILSAFE_SPEED FAN_SPEED_LOW
#endif
#ifndef FAN_ESCALATION_INTERVAL_MIN
#define FAN_ESCALATION_INTERVAL_MIN 10
#endif
#ifndef FAN_ESCALATION_DROP_C
#define FAN_ESCALATION_DROP_C 0.5f
#endif
// ---------------------------------------------------------------------------
// Thresholds do potenciômetro
// ---------------------------------------------------------------------------
// ADC abaixo deste valor é interpretado como "mínimo" (ventoinha off via pot)
static const int POT_MIN_ADC    = 80;    // em unidades de 0–4095
// Histerese para sair da região de "mínimo" e evitar comutações por ruído
static const int POT_MIN_EXIT_ADC = POT_MIN_ADC + 40;
// Variação mínima no ADC para considerar que o usuário moveu o pot intencionalmente
static const int POT_CHANGE_ADC = 60;

// ---------------------------------------------------------------------------
// Modos de operação
// ---------------------------------------------------------------------------
/**
 * Modo de operação da ventoinha.
 * FAN_MODE_AUTO:          Controle automático por temperatura (histérese + cooldown).
 * FAN_MODE_MANUAL_OFF:    Desligada manualmente (pot ou web) — aguarda próximo RTC_ON.
 * FAN_MODE_MANUAL_SPEED:  Velocidade fixada manualmente (pot ou web) — aguarda próximo RTC_ON.
 */
enum FanMode {
  FAN_MODE_AUTO,
  FAN_MODE_MANUAL_OFF,
  FAN_MODE_MANUAL_SPEED,
};

/**
 * Sub-estado interno do modo AUTO.
 * FAN_AUTO_IDLE:      Temperatura abaixo do trigger — ventoinha desligada.
 * FAN_AUTO_RUNNING:   Temperatura acima do trigger — ventoinha controlando.
 * FAN_AUTO_COOLDOWN:  Temperatura abaixo de TEMP_FAN_OFF — aguardando cooldown.
 */
enum FanAutoState {
  FAN_AUTO_IDLE,
  FAN_AUTO_RUNNING,
  FAN_AUTO_COOLDOWN,
};

// ---------------------------------------------------------------------------
// Estado global
// ---------------------------------------------------------------------------
static FanMode      _mode      = FAN_MODE_AUTO;
static FanAutoState _auto_st   = FAN_AUTO_IDLE;
static int          _speed_pct = 0;

// Cooldown: instante de início e duração calculada a partir de FAN_COOLDOWN_MIN
static unsigned long _cooldown_start_ms  = 0;
static const unsigned long COOLDOWN_MS   = (unsigned long)FAN_COOLDOWN_MIN * 60UL * 1000UL;

// Velocidade manual lembrada para o toggle (liga na última velocidade usada)
static int _last_manual_pct = 50;

// Tacômetro: contagem de pulsos via ISR e RPM calculado a cada janela de amostragem
static volatile uint32_t _tach_pulses    = 0;
static uint32_t          _tach_sample_ms = 0;
static int               _rpm            = 0;

// Escalonamento progressivo no modo AUTO RUNNING: piso mínimo de velocidade quando não há
// queda térmica suficiente após FAN_ESCALATION_INTERVAL_MIN minutos.
// Zerado ao entrar em cooldown (temperatura caiu) ou no RTC_ON.
static int           _escalation_floor_pct = 0;
static float         _escalation_ref_temp  = 0.0f;
static unsigned long _escalation_ref_ms    = 0;

// ---------------------------------------------------------------------------
// Calibração do potenciômetro
// ---------------------------------------------------------------------------
// Após um reset para AUTO (RTC_ON), o pot é ignorado até que o usuário:
//   1. Leve o pot ao mínimo (_pot_seen_min = true)
//   2. Aumente a partir do mínimo (_pot_calibrated = true)
// Isso evita que a posição residual do pot interfira no modo automático.
static bool _pot_seen_min    = false;
static bool _pot_calibrated  = false;
static int  _pot_adc_prev    = -1;
static int  _pot_filtered_adc = -1;
static bool _pot_min_latched  = false;

// Modo de segurança quando a temperatura fica indisponível/stale.
static bool _temp_failsafe_active = false;
static float _auto_trigger_c = TEMP_FAN_TRIGGER;
static float _auto_off_c     = TEMP_FAN_OFF;

// ---------------------------------------------------------------------------
// Helpers privados
// ---------------------------------------------------------------------------

/**
 * @brief Aplica percentual lógico (0–100) diretamente como duty cycle LEDC.
 *        Fan 4 pinos: o controlador interno da fan gerencia partida e velocidade mínima.
 */
static void _apply_speed(int pct) {
  pct        = constrain(pct, 0, 100);
  _speed_pct = pct;
  ledcWrite(FAN_LEDC_TARGET, (int)((long)pct * 255L / 100L));
}

/**
 * @brief ISR do tacômetro: incrementa contador a cada pulso de queda (falling edge).
 *        A fan gera 2 pulsos por revolução nesse pino open-drain (pino 3 do conector).
 */
static void IRAM_ATTR _tach_isr() {
  _tach_pulses++;
}

/**
 * @brief Retorna a velocidade alvo para o modo AUTO com base no delta de temperatura.
 * @param delta  temperatura_atual − TEMP_FAN_TRIGGER (sempre > 0 quando chamado)
 * @return percentual de velocidade (FAN_SPEED_LOW … FAN_SPEED_MAX)
 */
static int _auto_speed(float delta) {
  if (delta <= 1.0f) return FAN_SPEED_LOW;
  if (delta <= 2.0f) return FAN_SPEED_MED;
  if (delta <= 3.0f) return FAN_SPEED_HIGH;
  return FAN_SPEED_MAX;
}

/**
 * @brief Registra uma transição para MANUAL_OFF: desliga a ventoinha e
 *        invalida a calibração do potenciômetro, exigindo novo ciclo min→cima.
 */
static void _enter_manual_off() {
  _mode           = FAN_MODE_MANUAL_OFF;
  _pot_calibrated = false;
  _pot_seen_min   = false;
  _pot_adc_prev   = -1;
  _pot_filtered_adc = -1;
  _pot_min_latched  = false;
  _temp_failsafe_active = false;
  _apply_speed(0);
}

static bool _thresholds_valid(float trigger_c, float off_c) {
  if (isnan(trigger_c) || isnan(off_c)) return false;
  if (trigger_c < FAN_TRIGGER_MIN_C || trigger_c > FAN_TRIGGER_MAX_C) return false;
  if (off_c < FAN_OFF_MIN_C || off_c > FAN_OFF_MAX_C) return false;
  return trigger_c > off_c;
}

static void _save_thresholds_nvs(float trigger_c, float off_c) {
  Preferences prefs;
  if (!prefs.begin(FAN_CFG_NS, false)) {
    Serial.println("[Fan] Falha ao abrir NVS para salvar thresholds");
    return;
  }
  prefs.putFloat(FAN_CFG_KEY_TRIGGER, trigger_c);
  prefs.putFloat(FAN_CFG_KEY_OFF, off_c);
  prefs.end();
}

static void _load_thresholds_nvs() {
  Preferences prefs;
  if (!prefs.begin(FAN_CFG_NS, true)) {
    Serial.println("[Fan] Falha ao abrir NVS. Usando thresholds de config.h");
    _auto_trigger_c = TEMP_FAN_TRIGGER;
    _auto_off_c     = TEMP_FAN_OFF;
    return;
  }

  float trig = prefs.getFloat(FAN_CFG_KEY_TRIGGER, TEMP_FAN_TRIGGER);
  float off  = prefs.getFloat(FAN_CFG_KEY_OFF, TEMP_FAN_OFF);
  prefs.end();

  if (!_thresholds_valid(trig, off)) {
    _auto_trigger_c = TEMP_FAN_TRIGGER;
    _auto_off_c     = TEMP_FAN_OFF;
    Serial.println("[Fan] Thresholds em NVS invalidos. Usando config.h");
    return;
  }

  _auto_trigger_c = trig;
  _auto_off_c     = off;
  Serial.printf("[Fan] Thresholds AUTO carregados: trigger=%.1fC off=%.1fC\n",
                _auto_trigger_c, _auto_off_c);
}

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------

void fan_init() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!ledcAttach(PIN_FAN, LEDC_FREQ_HZ, LEDC_RESOLUTION)) {
    Serial.println("[Fan] Falha ao configurar PWM LEDC");
    return;
  }
#else
  ledcSetup(LEDC_CHANNEL, LEDC_FREQ_HZ, LEDC_RESOLUTION);
  ledcAttachPin(PIN_FAN, LEDC_CHANNEL);
#endif
  pinMode(PIN_FAN_TACH, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_FAN_TACH), _tach_isr, FALLING);
  _tach_sample_ms = millis();
  _load_thresholds_nvs();
  _apply_speed(0);
  Serial.printf("[Fan] Inicializada — desligada (modo AUTO), tacometro ativo no GPIO%d\n", PIN_FAN_TACH);
}

void fan_on_rtc_reset() {
  // Chamado pelo RTC quando o período ON da luminária começa.
  // Reseta para AUTO e exige re-calibração do pot para evitar ativação acidental.
  _mode     = FAN_MODE_AUTO;
  _auto_st  = FAN_AUTO_IDLE;
  _pot_seen_min    = false;
  _pot_calibrated  = false;
  _pot_adc_prev    = -1;
  _pot_filtered_adc = -1;
  _pot_min_latched  = false;
  _temp_failsafe_active  = false;
  _escalation_floor_pct  = 0;
  // Não altera _speed_pct agora; fan_update() decidirá no próximo ciclo.
  Serial.println("[Fan] Modo AUTO restaurado (RTC_ON) — re-calibracao do potenciometro necessaria");
}

void fan_set_speed_web(int percent) {
  percent = constrain(percent, 0, 100);
  if (percent == 0) {
    _enter_manual_off();
    Serial.println("[Fan] Desligada via web (fan_set_speed 0)");
    return;
  }
  _last_manual_pct = percent;
  _mode = FAN_MODE_MANUAL_SPEED;
  _apply_speed(percent);
  Serial.printf("[Fan] Velocidade manual via web: %d%%\n", percent);
}

void fan_toggle_web() {
  if (_speed_pct == 0 || _mode == FAN_MODE_MANUAL_OFF) {
    // Estava desligada: liga na última velocidade manual
    _mode = FAN_MODE_MANUAL_SPEED;
    _apply_speed(_last_manual_pct);
    Serial.printf("[Fan] Ligada via web (%d%%)\n", _last_manual_pct);
  } else {
    // Estava ligada: desliga manualmente
    _last_manual_pct = _speed_pct;   // memoriza a velocidade atual para próximo toggle-on
    _enter_manual_off();
    Serial.println("[Fan] Desligada via web (MANUAL_OFF)");
  }
}

int fan_get_speed_percent() {
  return _speed_pct;
}

bool fan_is_on() {
  return _speed_pct > 0;
}

int fan_get_rpm() {
  return _rpm;
}

bool fan_set_auto_thresholds(float trigger_c, float off_c) {
  if (!_thresholds_valid(trigger_c, off_c)) {
    return false;
  }

  _auto_trigger_c = trigger_c;
  _auto_off_c     = off_c;
  _save_thresholds_nvs(trigger_c, off_c);

  log_eventf("[Fan] Thresholds atualizados: trigger=%.1fC off=%.1fC",
             _auto_trigger_c, _auto_off_c);
  Serial.printf("[Fan] Thresholds AUTO atualizados: trigger=%.1fC off=%.1fC\n",
                _auto_trigger_c, _auto_off_c);
  return true;
}

float fan_get_trigger_c() {
  return _auto_trigger_c;
}

float fan_get_off_c() {
  return _auto_off_c;
}

void fan_update() {
  // ── 0. Tacômetro: amostragem a cada 2 s ─────────────────────────────────
  uint32_t _now = millis();
  if (_now - _tach_sample_ms >= 2000UL) {
    noInterrupts();
    uint32_t pulses = _tach_pulses;
    _tach_pulses = 0;
    interrupts();
    uint32_t elapsed = _now - _tach_sample_ms;
    _tach_sample_ms  = _now;
    // 2 pulsos por volta; elapsed em ms → rpm = pulses * 30000 / elapsed
    _rpm = elapsed > 0 ? (int)((pulses * 30000UL) / elapsed) : 0;
  }

  // ── 1. Potenciômetro ─────────────────────────────────────────────────────
  int pot_raw = analogRead(PIN_POT);
  if (_pot_filtered_adc < 0) {
    _pot_filtered_adc = pot_raw;
  } else {
    // Filtro exponencial simples para reduzir ruído no ADC.
    _pot_filtered_adc = (_pot_filtered_adc * 3 + pot_raw) / 4;
  }

  int pot_adc = _pot_filtered_adc;
  if (!_pot_min_latched) {
    if (pot_adc <= POT_MIN_ADC) {
      _pot_min_latched = true;
    }
  } else if (pot_adc >= POT_MIN_EXIT_ADC) {
    _pot_min_latched = false;
  }
  bool pot_at_min = _pot_min_latched;

  if (!_pot_calibrated) {
    // Fase de calibração: aguarda o usuário ir ao mínimo e depois subir
    if (pot_at_min) {
      _pot_seen_min = true;
    } else if (_pot_seen_min) {
      // Saiu do mínimo após ter passado pelo mínimo: pot pronto para uso
      _pot_calibrated = true;
      _pot_adc_prev   = pot_adc;
      Serial.println("[Fan] Potenciometro calibrado");
    }
  } else {
    // Pot calibrado: interpreta posição
    if (pot_at_min) {
      // Usuário colocou no mínimo intencionalmente → MANUAL_OFF
      if (_mode != FAN_MODE_MANUAL_OFF) {
        _enter_manual_off();
        Serial.println("[Fan] Desligada via potenciometro (MANUAL_OFF)");
      }
    } else {
      // Verifica se houve mudança significativa (filtra ruído do ADC)
      if (_pot_adc_prev < 0 || abs(pot_adc - _pot_adc_prev) > POT_CHANGE_ADC) {
        _pot_adc_prev   = pot_adc;
        int new_pct     = (int)map(pot_adc, POT_MIN_ADC, 4095, 1, 100);
        new_pct         = constrain(new_pct, 1, 100);
        _last_manual_pct = new_pct;
        _mode            = FAN_MODE_MANUAL_SPEED;
        _apply_speed(new_pct);
      }
    }
  }

  // ── 2. Modo manual: nada mais a fazer ────────────────────────────────────
  if (_mode != FAN_MODE_AUTO) return;

  // ── 3. Modo AUTO: controle por temperatura com histérese ─────────────────
  if (!temperature_is_fresh()) {
    int failsafe_pct = constrain(FAN_FAILSAFE_SPEED, 0, 100);
    if (!_temp_failsafe_active || _speed_pct != failsafe_pct) {
      _apply_speed(failsafe_pct);
      _temp_failsafe_active = true;
      Serial.printf("[Fan] AUTO em seguranca: temperatura indisponivel/stale, mantendo %d%%\n",
                    failsafe_pct);
    }
    return;
  }

  _temp_failsafe_active = false;
  float temp = temperature_read();

  switch (_auto_st) {

    case FAN_AUTO_IDLE:
      // Aguardando temperatura subir acima do trigger
      if (temp > _auto_trigger_c) {
        float delta = temp - _auto_trigger_c;
        int   speed = max(_auto_speed(delta), _escalation_floor_pct);
        _apply_speed(speed);
        _auto_st             = FAN_AUTO_RUNNING;
        _escalation_ref_temp = temp;
        _escalation_ref_ms   = millis();
        Serial.printf("[Fan] AUTO IDLE→RUNNING: %.1f°C (Δ%.1f°C) → %d%%\n",
                      temp, delta, speed);
      }
      break;

    case FAN_AUTO_RUNNING:
      if (temp < _auto_off_c) {
        // Temperatura caiu abaixo do limiar: inicia cooldown e reseta piso de escalonamento
        _cooldown_start_ms    = millis();
        _escalation_floor_pct = 0;
        _apply_speed(FAN_SPEED_LOW);
        _auto_st = FAN_AUTO_COOLDOWN;
        Serial.printf("[Fan] AUTO RUNNING→COOLDOWN: %.1f°C — %d min, %d%%\n",
                      temp, FAN_COOLDOWN_MIN, FAN_SPEED_LOW);
      } else if (temp >= _auto_trigger_c) {
        // Acima do trigger: ajusta velocidade por delta, respeitando o piso de escalonamento
        float delta = temp - _auto_trigger_c;
        int   speed = max(_auto_speed(delta), _escalation_floor_pct);
        if (speed != _speed_pct) {
          _apply_speed(speed);
          Serial.printf("[Fan] AUTO velocidade ajustada: %.1f°C (Δ%.1f°C) → %d%% (piso %d%%)\n",
                        temp, delta, speed, _escalation_floor_pct);
        }

        // Escalonamento progressivo: verifica progresso térmico a cada intervalo configurado
        unsigned long now_esc = millis();
        if (now_esc - _escalation_ref_ms >= (unsigned long)FAN_ESCALATION_INTERVAL_MIN * 60000UL) {
          float drop = _escalation_ref_temp - temp;
          if (drop < (float)FAN_ESCALATION_DROP_C && _escalation_floor_pct < FAN_SPEED_MAX) {
            // Sem queda suficiente: avança o piso para o próximo degrau acima do nível atual
            int next_floor;
            if      (_speed_pct < FAN_SPEED_LOW)  next_floor = FAN_SPEED_LOW;
            else if (_speed_pct < FAN_SPEED_MED)  next_floor = FAN_SPEED_MED;
            else if (_speed_pct < FAN_SPEED_HIGH) next_floor = FAN_SPEED_HIGH;
            else                                   next_floor = FAN_SPEED_MAX;
            if (next_floor > _escalation_floor_pct) {
              _escalation_floor_pct = next_floor;
              Serial.printf("[Fan] AUTO escalonamento: queda %.1fC abaixo do minimo (%.1fC) → piso %d%%\n",
                            drop, (float)FAN_ESCALATION_DROP_C, _escalation_floor_pct);
            }
          }
          _escalation_ref_temp = temp;
          _escalation_ref_ms   = now_esc;
        }
      } else {
        // Banda de histérese (TEMP_FAN_OFF ≤ temp < TEMP_FAN_TRIGGER):
        // temperatura aceitável — mantém velocidade atual e reinicia referência de escalonamento
        _escalation_ref_temp = temp;
        _escalation_ref_ms   = millis();
      }
      break;

    case FAN_AUTO_COOLDOWN:
      if (temp > _auto_trigger_c) {
        // Temperatura subiu novamente durante o cooldown: cancela e retoma controle
        float delta = temp - _auto_trigger_c;
        int   speed = max(_auto_speed(delta), _escalation_floor_pct);
        _apply_speed(speed);
        _auto_st             = FAN_AUTO_RUNNING;
        _escalation_ref_temp = temp;
        _escalation_ref_ms   = millis();
        Serial.printf("[Fan] AUTO COOLDOWN→RUNNING: temperatura voltou (%.1f°C) → %d%%\n",
                      temp, speed);
      } else if (millis() - _cooldown_start_ms >= COOLDOWN_MS) {
        // Cooldown expirado: desliga
        _apply_speed(0);
        _auto_st = FAN_AUTO_IDLE;
        Serial.println("[Fan] AUTO COOLDOWN→IDLE: cooldown encerrado, desligada");
      }
      break;
  }
}
