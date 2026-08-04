#include "light.h"
#include "config.h"

static bool          _state             = false;
static bool          _btn_prev_state    = HIGH;
static unsigned long _btn_last_debounce = 0;

static const unsigned long DEBOUNCE_MS = 50;

void light_init() {
  pinMode(PIN_SSR, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  light_set(false);
  Serial.println("[Light] Inicializada — desligada");
}

void light_set(bool on) {
  digitalWrite(PIN_SSR, on ? HIGH : LOW);
  _state = on;
  Serial.print("[Light] ");
  Serial.println(on ? "LIGADA" : "DESLIGADA");
}

void light_toggle() {
  light_set(!_state);
}

bool light_get_state() {
  return _state;
}

void light_check_button() {
  bool reading = digitalRead(PIN_BUTTON);

  // Detecta borda de descida (botão pressionado — pino vai de HIGH para LOW com INPUT_PULLUP)
  if (reading == LOW && _btn_prev_state == HIGH) {
    unsigned long now = millis();
    if (now - _btn_last_debounce > DEBOUNCE_MS) {
      _btn_last_debounce = now;
      light_toggle();
      Serial.println("[Light] Alternada via botão físico");
    }
  }

  _btn_prev_state = reading;
}
