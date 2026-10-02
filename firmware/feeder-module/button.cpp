#include "button.h"

#include <Arduino.h>
#include <esp_timer.h>

#include "config.h"
#include "debuglog.h"

struct ButtonEvent {
  bool     pressed;
  uint32_t at;
};

// Fila de um produtor (o timer) e um consumidor (o loop).
static const uint8_t FILA = 8;
static ButtonEvent       _fila[FILA];
static volatile uint8_t  _escrita = 0;
static volatile uint8_t  _leitura = 0;

static bool    _estado = false;    // pressionado, já sem trepidação
static uint8_t _seguidas = 0;      // amostras seguidas no nível novo

static void _amostra(void*) {
  bool agora = digitalRead(PIN_BUTTON) == LOW;   // INPUT_PULLUP: pressionado = baixo
  if (agora == _estado) {
    _seguidas = 0;
    return;
  }
  if (++_seguidas < BUTTON_STABLE_MS / BUTTON_SAMPLE_MS) return;
  _seguidas = 0;
  _estado = agora;
  uint8_t prox = (uint8_t)((_escrita + 1) % FILA);
  if (prox == _leitura) return;   // cheia: o loop parou? descarta em vez de sobrescrever
  _fila[_escrita] = {agora, millis()};
  _escrita = prox;
}

void button_init() {
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  esp_timer_create_args_t args = {};
  args.callback = _amostra;
  args.name = "botao";
  esp_timer_handle_t timer;
  if (esp_timer_create(&args, &timer) == ESP_OK) {
    esp_timer_start_periodic(timer, (uint64_t)BUTTON_SAMPLE_MS * 1000ULL);
  } else {
    Log.println("[Botao] Timer de amostragem nao subiu; o botao fica sem efeito");
  }
}

void button_poll(GestureDetector& g) {
  while (_leitura != _escrita) {
    ButtonEvent e = _fila[_leitura];
    _leitura = (uint8_t)((_leitura + 1) % FILA);
    if (e.pressed) {
      g.press(e.at);
    } else {
      g.release(e.at);
    }
  }
}
