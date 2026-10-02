#pragma once
#include <stdint.h>

#include "params.h"

enum class Gesture : uint8_t { NONE, TAP1, TAP2, TAP3, LONG };

/**
 * Pressões e solturas — já sem trepidação, vindas do amostrador do botão —
 * viram gestos:
 *
 * - toques: contados enquanto o próximo começa em menos de `TAP_WINDOW_MS`
 *   depois da soltura anterior; o gesto sai quando a janela fecha (1, 2 ou 3;
 *   mais que 3 conta como 3);
 * - longo: `LONG_PRESS_MS` segurando. Sai **enquanto** o dedo ainda está no
 *   botão — quem segura vê a tela mudar, sem precisar adivinhar quando soltar
 *   —, e a soltura dele não conta como toque.
 *
 * Lógica pura, sem relógio: o tempo vem de quem chama.
 */
class GestureDetector {
 public:
  void press(uint32_t now);
  void release(uint32_t now);
  /** A cada volta do loop: devolve um gesto quando ele se completa. */
  Gesture update(uint32_t now);
  bool pressed() const { return _pressed; }

 private:
  bool     _pressed = false;
  bool     _long_fired = false;
  uint32_t _press_at = 0;
  uint32_t _release_at = 0;
  uint8_t  _taps = 0;
};
