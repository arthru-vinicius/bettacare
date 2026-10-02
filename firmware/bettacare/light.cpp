#include "light.h"

#include <esp_timer.h>
#include <soc/gpio_struct.h>

#include "config.h"
#include "event_log.h"

static bool        _state  = false;
static LightSource _source = LIGHT_SRC_BOOT;

/** Um botão de verdade não fica pressionado meio minuto. */
static const unsigned long STUCK_MS = 30000;
static bool _btn_stuck_reported = false;

/**
 * Debounce por amostragem de nível, não por borda (UPGRADE/07).
 *
 * A versão por interrupção aceitava como toque **qualquer** borda de descida
 * no GPIO18, e o "debounce" de 50 ms era inócuo porque o loop roda a cada
 * 200 ms — toda borda de uma volta nova passava. Medido em campo: cada
 * bit-slot do 1-Wire no GPIO19 vizinho induz um pico de microssegundos no
 * GPIO18 (16 picos no pedido de conversão do DS18B20, 152 na leitura, a cada
 * 5 s), e cada rajada virava uma troca do relé — o "pisca sozinho a cada 5
 * segundos". O mesmo defeito fazia a soltura do botão contar como um segundo
 * toque ("segurando acende, soltando apaga").
 *
 * Agora um timer amostra o nível a cada 2 ms: a pressão só conta depois de
 * 30 ms contínuos em LOW, e a próxima só depois de 30 ms contínuos em HIGH.
 * Pico de microssegundos nunca soma 15 amostras seguidas; toque humano (80 ms
 * ou mais) sempre soma — e toque curto continua sendo pego, que era o motivo
 * de a interrupção existir.
 */
static const uint64_t SAMPLE_PERIOD_US = 2000;
static const uint8_t  PRESS_SAMPLES    = 15;   // 30 ms
static const uint8_t  RELEASE_SAMPLES  = 15;   // 30 ms

static esp_timer_handle_t _sampler         = nullptr;
static uint8_t            _low_run         = 0;   // só o timer escreve
static uint8_t            _high_run        = 0;   // só o timer escreve
static volatile bool      _pressed         = false;
static volatile uint32_t  _pressed_since_ms = 0;
static volatile uint32_t  _presses          = 0;  // pressões confirmadas desde o boot
static uint32_t           _presses_seen     = 0;  // só o loop escreve

static void _sample_button(void*) {
  bool baixo = ((GPIO.in >> PIN_BUTTON) & 0x1) == 0;
  if (baixo) {
    _high_run = 0;
    if (_low_run < 255) _low_run++;
    if (!_pressed && _low_run >= PRESS_SAMPLES) {
      _pressed_since_ms = millis();
      _pressed = true;
      _presses = _presses + 1;
    }
  } else {
    _low_run = 0;
    if (_high_run < 255) _high_run++;
    if (_pressed && _high_run >= RELEASE_SAMPLES) _pressed = false;
  }
}

void light_init() {
  pinMode(PIN_SSR, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  esp_timer_create_args_t args = {};
  args.callback = &_sample_button;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "btn_sample";
  args.skip_unhandled_events = true;
  if (esp_timer_create(&args, &_sampler) != ESP_OK ||
      esp_timer_start_periodic(_sampler, SAMPLE_PERIOD_US) != ESP_OK) {
    event_log(SEV_FATAL, COMP_BUTTON, "button.sampler_failed",
              "Nao foi possivel iniciar a amostragem do botao");
  }

  light_set(false, LIGHT_SRC_BOOT);
}

/** Estado já filtrado — um pico de microssegundos não aparece como pressão. */
bool light_button_pressed() { return _pressed; }

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
  return _pressed && (millis() - _pressed_since_ms) > STUCK_MS;
}

void light_check_button() {
  // Cada pressão confirmada alterna uma vez. Normalmente é no máximo uma por
  // volta do loop — uma pressão inteira (30 ms baixo + 30 ms alto) não cabe
  // duas vezes em 200 ms na mão de ninguém.
  uint32_t confirmadas = _presses;
  while (_presses_seen != confirmadas) {
    _presses_seen++;
    light_set(!_state, LIGHT_SRC_BUTTON);
  }

  if (!_pressed) {
    _btn_stuck_reported = false;
  } else if (!_btn_stuck_reported && light_button_stuck()) {
    // Só registra uma vez por episódio: um botão com mau contato geraria um
    // evento a cada 200 ms e afogaria o log.
    _btn_stuck_reported = true;
    event_log(SEV_WARN, COMP_BUTTON, "button.stuck",
              "Botao pressionado ha mais de %lus; verifique o contato",
              STUCK_MS / 1000);
  }
}
