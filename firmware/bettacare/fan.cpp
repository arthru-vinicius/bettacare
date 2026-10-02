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
// 110: meio-termo entre o 80 original (forçava contra o batente) e o 300
// seguinte (margem grande demais, comia faixa útil de velocidade).
static const int POT_MIN_ADC      = 110;                 // 0–4095
static const int POT_MIN_EXIT_ADC = POT_MIN_ADC + 40;    // histerese de saída
/** Com o pot no comando: variação que vale como movimento e muda a velocidade. */
static const int POT_CHANGE_ADC   = 60;
/**
 * Sem o pot no comando — depois do boot, de um comando do app ou da virada da
 * agenda —, quanto ele precisa andar para tomar o comando de volta. Em duas
 * amostras seguidas: um pico de ruído do ADC não basta, e um giro de mão anda
 * centenas de pontos em menos de um segundo.
 */
static const int POT_TAKEOVER_ADC = 150;
static const uint8_t POT_TAKEOVER_SAMPLES = 2;
/** Conversões por amostra: a média corta o ruído do ADC sem atrasar a resposta. */
static const int POT_READS = 8;
/** O evento de velocidade sai quando a mão para, não a cada degrau do giro. */
static const unsigned long POT_SETTLE_MS = 1000;

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
static volatile uint32_t _tach_pulses     = 0;
static uint32_t          _tach_sample_ms  = 0;
static int               _rpm             = 0;
/** Pulsos crus da última janela — antes de virar RPM. Exposto no `diag`. */
static uint32_t          _tach_pulses_raw = 0;
static unsigned long     _stall_since_ms = 0;
static bool              _stall_reported = false;
/** Tempo com PWM e sem rotação antes de acusar defeito. */
static const unsigned long STALL_MS = 5000;
/**
 * Teto físico de RPM (UPGRADE/05, C1). O mesmo teto do contrato
 * (`fanStateSchema.rpm.max`) — se o tacômetro contar mais que isto, não é
 * rotação real, é ruído na linha (ver UPGRADE/02 §4, sobre o pull-up que
 * falta). Sem grampear aqui, o valor implausível derrubaria o POST inteiro no
 * servidor antes da correção de tolerância entrar em ação.
 */
static const int FAN_RPM_MAX = 20000;

// Escalonamento progressivo
static int           _escalation_floor_pct = 0;
static float         _escalation_ref_temp  = 0.0f;
static unsigned long _escalation_ref_ms    = 0;

// Potenciômetro — ver o comentário em `fan_update()`
static bool          _pot_armed       = false;   // o pot está no comando?
static int           _pot_adc         = -1;      // última amostra (média de POT_READS)
static int           _pot_ref         = -1;      // referência lenta, enquanto desarmado
static uint8_t       _pot_takeover    = 0;       // amostras seguidas longe da referência
static int           _pot_applied     = -1;      // leitura que gerou a velocidade atual
static bool          _pot_min_latched = false;
static bool          _pot_log_pending = false;
static unsigned long _pot_changed_ms  = 0;

static bool _temp_failsafe_active = false;

// ── Corte físico de energia (GPIO26) ─────────────────────────────────────────
//
// Ventoinha Tipo A: duty 0% só leva à rotação mínima própria dela, nunca para
// de verdade (ver §7 do guia de pinagem). Por isso o corte segue o duty
// diretamente, dentro de `_apply_speed()` — ligado sempre que pct > 0,
// cortado sempre que pct == 0 — em vez de casos especiais por chamador. Isso
// cobre IDLE automático, fim de COOLDOWN e desligar manual com a mesma regra,
// sem exigir que cada chamador lembre de cuidar da energia também.
static bool _power_on = true;
/** Um aviso por episódio, como o `fan.tach_stalled` — não um a cada 2 s. */
static bool _power_fault_reported = false;

static void _set_fan_power(bool on) {
  if (_power_on == on) return;
  _power_on = on;
  digitalWrite(PIN_FAN_POWER, on ? HIGH : LOW);
}

// ── Helpers ─────────────────────────────────────────────────────────────────

static void _apply_speed(int pct) {
  pct = constrain(pct, 0, 100);
  _speed_pct = pct;
  // O buffer MOSFET do PWM (pinagem-e-montagem-esp32.md §7, caso de 5V)
  // inverte o sinal: GPIO em alto liga o MOSFET, que puxa a linha da
  // ventoinha pra baixo — o oposto do dreno aberto direto. Sem o
  // complemento aqui, 100% comandado chega como 0% na ventoinha (ela gira no
  // mínimo) e 0% chega como 100% (ela nunca desliga) — era exatamente esse o
  // aviso que o documento já fazia e que este firmware nunca implementou.
  //
  // **Com a energia cortada, a linha fica solta** (duty 0: MOSFET do buffer
  // desligado). O corte é no retorno, o pino 1; o complemento de 0% deixava o
  // buffer conduzindo o tempo todo, com o pino 4 preso no GND — e a
  // eletrônica da ventoinha, sem o próprio terra, voltava por ele. Sobravam
  // 3 a 7 V nos terminais, e ela girava aos trancos, cadenciados, mesmo
  // "desligada" (bancada, 2026-10-02).
  if (pct > 0) {
    ledcWrite(FAN_LEDC_TARGET, (int)((long)(100 - pct) * 255L / 100L));
    _set_fan_power(true);
  } else {
    _set_fan_power(false);
    ledcWrite(FAN_LEDC_TARGET, 0);
  }
  if (pct == 0) {
    // Zera a vigilância do tacômetro: sem PWM, rotação zero é o esperado.
    _stall_since_ms = 0;
    _stall_reported = false;
  }
}

static void IRAM_ATTR _tach_isr() { _tach_pulses = _tach_pulses + 1; }

static int _auto_speed(float delta) {
  if (delta <= 1.0f) return FAN_SPEED_LOW;
  if (delta <= 2.0f) return FAN_SPEED_MED;
  if (delta <= 3.0f) return FAN_SPEED_HIGH;
  return FAN_SPEED_MAX;
}

/** Média de `POT_READS` conversões: corta o ruído sem o atraso de um filtro. */
static int _read_pot() {
  uint32_t soma = 0;
  for (int i = 0; i < POT_READS; i++) soma += (uint32_t)analogRead(PIN_POT);
  return (int)(soma / POT_READS);
}

/**
 * Tira o comando do pot: a posição física atual deixa de valer até ele ser
 * girado de novo. É o que impede o pot de desfazer, no ciclo seguinte, um
 * comando do app ou a volta ao automático da agenda — o último comando vence.
 */
static void _disarm_pot() {
  _pot_armed       = false;
  _pot_ref         = -1;   // a próxima amostra vira a referência
  _pot_takeover    = 0;
  _pot_applied     = -1;
  _pot_log_pending = false;
}

static void _enter_manual_off() {
  _mode = FAN_MODE_MANUAL_OFF;
  _temp_failsafe_active = false;
  _apply_speed(0); // já corta a energia — ver o comentário em `_apply_speed()`
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
  // OUTPUT + HIGH explícito aqui só confirma o que o pullup externo já
  // garante desde o boot: ventoinha energizada por padrão, mesmo antes desta
  // linha rodar.
  pinMode(PIN_FAN_POWER, OUTPUT);
  digitalWrite(PIN_FAN_POWER, HIGH);
  _tach_sample_ms = millis();
  _disarm_pot();
  _apply_speed(0);
}

void fan_on_schedule_reset() {
  _mode    = FAN_MODE_AUTO;
  _auto_st = FAN_AUTO_IDLE;
  _disarm_pot();
  _temp_failsafe_active = false;
  _escalation_floor_pct = 0;
  // Não mexe na velocidade (nem na energia) agora: `fan_update()` decide no
  // próximo ciclo, e `_apply_speed()` cuida da energia quando decidir.
}

void fan_set_speed(int percent) {
  percent = constrain(percent, 0, 100);
  _disarm_pot();
  if (percent == 0) {
    _enter_manual_off();
    event_log(SEV_INFO, COMP_FAN, "fan.off", "Ventoinha desligada por comando");
    return;
  }
  _last_manual_pct = percent;
  _mode = FAN_MODE_MANUAL_SPEED;
  _temp_failsafe_active = false;
  _apply_speed(percent); // > 0, então já religa a energia
  event_log(SEV_INFO, COMP_FAN, "fan.on",
            "Velocidade fixada em %d%% por comando", percent);
}

void fan_set_mode_auto(bool automatico) {
  _disarm_pot();
  if (automatico) {
    _mode    = FAN_MODE_AUTO;
    _auto_st = FAN_AUTO_IDLE;
    _escalation_floor_pct = 0;
    // Não aplica velocidade aqui — `fan_update()` decide (e cuida da
    // energia) no próximo ciclo, igual ao `fan_on_schedule_reset()`.
    event_log(SEV_INFO, COMP_FAN, "fan.mode_auto",
              "Controle automatico restaurado por comando");
  } else {
    _mode = FAN_MODE_MANUAL_SPEED;
    _temp_failsafe_active = false;
    _apply_speed(_last_manual_pct);
    event_log(SEV_INFO, COMP_FAN, "fan.mode_manual",
              "Modo manual por comando (%d%%)", _last_manual_pct);
  }
}

int  fan_get_speed_percent() { return _speed_pct; }
bool fan_is_on()             { return _speed_pct > 0; }
int  fan_get_rpm()           { return _rpm; }

int      fan_get_pot_raw_adc()     { return _pot_adc < 0 ? 0 : _pot_adc; }
uint32_t fan_get_tach_pulses_raw() { return _tach_pulses_raw; }

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
    _tach_pulses_raw = pulses;
    // 2 pulsos por volta → rpm = pulsos * 30000 / ms
    int rpm_bruto = elapsed > 0 ? (int)((pulses * 30000UL) / elapsed) : 0;

    // Fisicamente impossível vira ruído contado, não rotação real — grampeia
    // e avisa, em vez de deixar o número seguir para um POST que o servidor
    // teria de rejeitar inteiro (UPGRADE/05, C1).
    if (rpm_bruto > FAN_RPM_MAX) {
      event_log(SEV_WARN, COMP_FAN, "fan.tach_implausible",
                "Tacometro contou %d rpm, acima do fisicamente possivel; corrigido para %d",
                rpm_bruto, FAN_RPM_MAX);
      rpm_bruto = FAN_RPM_MAX;
    }
    _rpm = rpm_bruto;

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

    // Confere se o pino de corte de energia ainda obedece o que mandamos
    // escrever (UPGRADE/07 — o GPIO27 original queimou num curto gate/dreno
    // do AO3400 e ficou dias dando sintoma confuso de "ventoinha não desliga"
    // antes de alguém medir o pino direto com multímetro). Um pino morto
    // normalmente fica preso num nível só, então ler de volta aqui pega isso
    // na hora, em vez de deixar o comportamento estranho se arrastar.
    bool pino_obedece = (digitalRead(PIN_FAN_POWER) == HIGH) == _power_on;
    if (!pino_obedece && !_power_fault_reported) {
      _power_fault_reported = true;
      event_log(SEV_ERROR, COMP_FAN, "fan.power_pin_fault",
                "GPIO do corte de energia nao reflete o valor escrito; "
                "suspeite do pino ter queimado");
    } else if (pino_obedece && _power_fault_reported) {
      _power_fault_reported = false;
      event_log(SEV_INFO, COMP_FAN, "fan.power_pin_ok",
                "GPIO do corte de energia voltou a refletir o valor escrito");
    }
  }

  // ── 1. Potenciômetro ──────────────────────────────────────────────────────
  //
  // Lido a cada volta do loop (200 ms), na média de 8 conversões. Antes era
  // uma leitura a cada 500 ms com um filtro que levava ~6 s para chegar ao
  // valor novo: girar até o fim fazia a ventoinha "escorregar" de velocidade
  // por segundos, e o desligar no mínimo demorava o mesmo tanto.
  //
  // E antes o pot só tomava o comando depois de ir ao mínimo e subir — de
  // novo a cada boot (OTA incluída), comando do app e virada da agenda. Girar
  // de qualquer outro ponto não fazia nada, e parecia que a ventoinha não o
  // respeitava. Agora basta girar: andou mais que `POT_TAKEOVER_ADC` da
  // posição em que estava, ele assume, e a posição vale na mesma volta.
  int pot = _read_pot();
  _pot_adc = pot;

  if (!_pot_armed) {
    if (_pot_ref < 0) {
      _pot_ref = pot;
    } else if (abs(pot - _pot_ref) > POT_TAKEOVER_ADC) {
      if (++_pot_takeover >= POT_TAKEOVER_SAMPLES) {
        _pot_armed       = true;
        _pot_applied     = -1;   // aplica já, nesta volta
        _pot_min_latched = pot <= POT_MIN_ADC;
      }
    } else {
      _pot_takeover = 0;
      // Referência lenta: absorve a deriva do ADC com a temperatura, nunca um
      // giro de mão.
      _pot_ref += (pot - _pot_ref) / 32;
    }
  }

  if (_pot_armed) {
    if (!_pot_min_latched) {
      if (pot <= POT_MIN_ADC) _pot_min_latched = true;
    } else if (pot >= POT_MIN_EXIT_ADC) {
      _pot_min_latched = false;
    }

    if (_pot_min_latched) {
      if (_mode != FAN_MODE_MANUAL_OFF) {
        // Sem `_disarm_pot()`: quem desligou foi o pot, e ele segue no comando.
        _enter_manual_off();
        _pot_log_pending = false;
        // Código próprio da família `pot.*` (UPGRADE/03, F12): antes usava
        // `fan.off`, e quem filtrasse por componente `pot` via `code=fan.*`,
        // ou agregasse por código, misturava ação do potenciômetro com ação
        // da ventoinha automática.
        event_log(SEV_INFO, COMP_POT, "pot.fan_off",
                  "Ventoinha desligada pelo potenciometro");
      }
      _pot_applied = pot;
    } else if (_mode != FAN_MODE_MANUAL_SPEED || _pot_applied < 0 ||
               abs(pot - _pot_applied) > POT_CHANGE_ADC) {
      _pot_applied = pot;
      int novo = constrain((int)map(pot, POT_MIN_ADC, 4095, 1, 100), 1, 100);
      _last_manual_pct = novo;
      _mode = FAN_MODE_MANUAL_SPEED;
      _temp_failsafe_active = false;
      _apply_speed(novo); // > 0, então já religa a energia
      _pot_log_pending = true;
      _pot_changed_ms  = agora;
    }

    // Um evento quando a mão para, não um a cada degrau do giro — eram 184
    // `pot.fan_speed` num só dia de bancada.
    if (_pot_log_pending && agora - _pot_changed_ms >= POT_SETTLE_MS) {
      _pot_log_pending = false;
      event_log(SEV_INFO, COMP_POT, "pot.fan_speed",
                "Velocidade ajustada para %d%% pelo potenciometro", _speed_pct);
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
      } else if (_speed_pct != 0) {
        // IDLE precisa impor velocidade zero, não só deixar de subir: sem
        // isto, sair do failsafe (que aplica FAN_FAILSAFE_SPEED direto, sem
        // passar por RUNNING) ou trocar pra automático vindo de uma
        // velocidade manual deixava a ventoinha "presa" na velocidade
        // anterior indefinidamente, porque nada aqui jamais mandava zerar.
        _apply_speed(0);
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
