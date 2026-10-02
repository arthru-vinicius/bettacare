#include "gestures.h"

void GestureDetector::press(uint32_t now) {
  _pressed = true;
  _press_at = now;
  _long_fired = false;
}

void GestureDetector::release(uint32_t now) {
  _pressed = false;
  if (_long_fired) {
    _taps = 0;
    return;
  }
  if (_taps < 3) _taps++;
  _release_at = now;
}

Gesture GestureDetector::update(uint32_t now) {
  if (_pressed && !_long_fired && now - _press_at >= LONG_PRESS_MS) {
    _long_fired = true;
    _taps = 0;   // toques antes de segurar são descartados
    return Gesture::LONG;
  }
  if (!_pressed && _taps > 0 && now - _release_at >= TAP_WINDOW_MS) {
    Gesture g = _taps == 1 ? Gesture::TAP1 : _taps == 2 ? Gesture::TAP2 : Gesture::TAP3;
    _taps = 0;
    return g;
  }
  return Gesture::NONE;
}
