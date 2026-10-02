#pragma once

#include <Arduino.h>

/**
 * Enlace serial com o módulo opcional de alimentação de precisão.
 *
 * O módulo (ESP32-C3) roda sozinho, com RTC, botão e OLED próprios — este
 * arquivo não sabe nada sobre grãos, servo ou motor de vibração. A única
 * responsabilidade daqui é a ponte UART2 (GPIO4/GPIO16, ver
 * `pinagem-e-montagem-esp32.md` §8): mandar `FEED`/`CFG`, ouvir
 * `PONG`/`FED`/`SCHEDULE`, e decidir se o módulo está "conectado" — que é só
 * "respondeu recentemente", nunca uma condição de erro por si só. O módulo
 * não fica ligado o tempo todo por desenho (ver `pinagem-alimentador-modulo.md`),
 * então desconectado é o estado comum, não uma falha.
 *
 * Roda inteiro no núcleo de controle (chamado do `loop()`, como light/fan) —
 * sem mutex próprio porque não há segundo escritor. `_publish_snapshot()` lê
 * o estado daqui de forma síncrona, no mesmo núcleo, antes de publicar no
 * `app_state` — é o `app_state` que cruza para o núcleo de rede.
 *
 * Protocolo, uma linha ASCII por mensagem, terminada em `\n`:
 *
 *   ESP32 → módulo   PING
 *                     FEED [grãos]        grãos omitido = padrão do módulo
 *                     CFG <h1> <h2> <grãos> <0|1>
 *
 *   módulo → ESP32   PONG <h1> <h2> <grãos> <auto> <idade_s> <req> <conf> <ok>
 *                     SCHEDULE <h1> <h2> <grãos> <auto>   — empurrado sozinho a cada reconexão
 *                     FED <req> <conf> <ok>                — resultado de uma alimentação
 */

struct FeederLinkState {
  bool     connected;
  bool     auto_enabled;
  uint8_t  hour1;
  uint8_t  hour2;
  uint8_t  grains_per_feeding;
  /** `UINT32_MAX` = nunca alimentou desde que conhecemos o módulo. */
  uint32_t last_feed_age_s;
  uint8_t  last_feed_requested;
  uint8_t  last_feed_confirmed;
  bool     last_feed_ok;
};

void feeder_link_init();

/** Chamado a cada ciclo do loop: drena o UART, manda PING periódico, detecta timeout. */
void feeder_link_update();

/** Pede uma alimentação manual. `grains == 0` usa o padrão configurado no módulo. */
void feeder_link_request_feed(uint8_t grains);

/** Substitui a agenda inteira do módulo — nunca um campo isolado (ver contrato, `feeder.set_config`). */
void feeder_link_request_config(uint8_t hour1, uint8_t hour2,
                                 uint8_t grains_per_feeding, bool auto_enabled);

/** Cópia do estado conhecido — sempre segura de chamar, mesmo com o módulo ausente. */
FeederLinkState feeder_link_get_state();
