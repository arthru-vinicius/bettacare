#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * O que aconteceu durante um período sem contato com o servidor.
 *
 * Nasceu do apagão de 5,8 h de 2026-10-13: na volta, a única pista no banco
 * era um `api.post_failed` com `repeat_count` 6 — não dava para saber se o
 * Wi-Fi tinha caído, se o servidor recusava, nem com que sinal. Agora a volta
 * traz um resumo: quanto durou, quantas tentativas falharam e de que jeito,
 * quanto tempo sem Wi-Fi, quantas quedas e por quê, e a faixa de RSSI.
 *
 * Lógica pura, sem Arduino: a task de rede alimenta, `on_success()` devolve o
 * texto do evento `api.outage_summary`. Testada no PC por
 * `firmware/test/test_outage_tracker.cpp`.
 */
class OutageTracker {
 public:
  /**
   * POST que falhou com o Wi-Fi de pé. `http_status` negativo é erro de rede do
   * HTTPClient. `free_heap` (0 = não medido) entra no resumo como o mínimo do
   * período: o heap de 756 bytes visto em produção (2026-10) não tinha como
   * dizer em que momento aconteceu.
   */
  void on_failure(uint32_t now_ms, int http_status, int16_t rssi, bool rssi_valid,
                  uint32_t free_heap = 0);

  /** POST que nem saiu: sem Wi-Fi. Chamado a cada ciclo da task de rede. */
  void on_no_wifi(uint32_t now_ms);

  /** O Wi-Fi caiu (motivo `wifi_err_reason_t` do ESP-IDF). */
  void on_wifi_drop(uint32_t now_ms, uint8_t reason);

  /**
   * POST aceito. Se ele encerrou um período sem contato de pelo menos
   * `min_ms`, escreve o resumo em `out` e devolve verdadeiro.
   */
  bool on_success(uint32_t now_ms, uint32_t min_ms, char* out, size_t len);

  bool active() const { return _active; }

 private:
  bool     _active = false;
  /** Último POST aceito antes do período — é daí que se conta o silêncio. */
  uint32_t _last_ok_ms = 0;
  bool     _has_last_ok = false;
  uint32_t _start_ms = 0;

  uint16_t _timeouts = 0;   // -11: a requisição saiu, a resposta não voltou
  uint16_t _conn = 0;       // -1: nem conectou
  uint16_t _other = 0;      // qualquer outro código, inclusive 5xx
  int      _last_other = 0;     // o último deles (só o negativo, de rede, entra no texto)
  uint32_t _heap_min = 0;
  bool     _has_heap = false;
  uint32_t _no_wifi_ms = 0;
  uint32_t _last_no_wifi_ms = 0;
  uint16_t _drops = 0;
  uint8_t  _last_reason = 0;
  int16_t  _rssi_min = 0;
  int16_t  _rssi_max = 0;
  bool     _has_rssi = false;

  void _begin(uint32_t now_ms);
};
