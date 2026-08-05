#include "light.h"

#include "config.h"
#include "event_log.h"

static bool          _state          = false;
static LightSource   _source         = LIGHT_SRC_BOOT;
static bool          _btn_prev       = HIGH;
static unsigned long _btn_last_debounce = 0;
static unsigned long _btn_low_since  = 0;
static bool          _btn_stuck_reported = false;

static const unsigned long DEBOUNCE_MS = 50;
/** Um botão de verdade não fica pressionado meio minuto. */
static const unsigned long STUCK_MS = 30000;

void light_init() {
  pinMode(PIN_SSR, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  light_set(false, LIGHT_SRC_BOOT);
}

void light_set(bool on, LightSource source) {
  bool mudou = (on != _state);
  digitalWrite(PIN_SSR, on ? HIGH : LOW);
  _state = on;
  _source = source;

  if (mudou) {
    event_log(SEV_INFO, COMP_LIGHT, on ? "light.on" : "light.off",
              "Luminaria %s (%s)", on ? "acesa" : "apagada",
              light_source_name(source));
  }
}

bool light_get_state() { return _state; }
LightSource light_get_source() { return _source; }

bool light_button_stuck() {
  return _btn_low_since != 0 && (millis() - _btn_low_since) > STUCK_MS;
}

void light_check_button() {
  bool reading = digitalRead(PIN_BUTTON);
  unsigned long agora = millis();

  // Borda de descida: com INPUT_PULLUP, pressionar leva o pino a LOW.
  if (reading == LOW && _btn_prev == HIGH) {
    if (agora - _btn_last_debounce > DEBOUNCE_MS) {
      _btn_last_debounce = agora;
      _btn_low_since = agora;
      light_set(!_state, LIGHT_SRC_BUTTON);
    }
  }

  if (reading == HIGH) {
    _btn_low_since = 0;
    _btn_stuck_reported = false;
  } else if (!_btn_stuck_reported && light_button_stuck()) {
    // Só registra uma vez por episódio: um botão com mau contato geraria um
    // evento a cada 200 ms e afogaria o log.
    _btn_stuck_reported = true;
    event_log(SEV_WARN, COMP_BUTTON, "button.stuck",
              "Botao pressionado ha mais de %lus; verifique o contato",
              STUCK_MS / 1000);
  }

  _btn_prev = reading;
}
