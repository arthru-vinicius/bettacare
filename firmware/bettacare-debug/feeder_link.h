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
 * **Presença física antes de qualquer conversa.** O RX (GPIO16) tem pull-down:
 * conector vazio é nível baixo permanente, e um módulo ligado o mantém em alto
 * — o repouso de qualquer UART. Sem módulo no fio, o principal não manda nem
 * `PING`, e o POST não leva bloco `feeder`. Do lado do módulo, isso exige o TX
 * dele ligado ao fio por no máximo ~2,2 kΩ em série (ver
 * `pinagem-alimentador-modulo.md`): mais que isso, o pull-down interno de
 * ~45 kΩ puxa o nível alto para perto do limiar.
 *
 * Protocolo, uma linha ASCII por mensagem, terminada em `\n`:
 *
 *   ESP32 → módulo   PING                       a cada 2 s, só com o módulo presente
 *                     FEED [grãos] [FORCE]      grãos omitido = padrão do módulo;
 *                                               FORCE ignora o limite de 24 h (só o app)
 *                     CFG <h1> <h2> <grãos> <0|1>
 *
 *   módulo → ESP32   PONG <h1> <h2> <grãos> <auto> <idade_s> <req> <conf> <ok> [<refeições_24h>]
 *                     SCHEDULE <h1> <h2> <grãos> <auto>   — empurrado sozinho a cada reconexão
 *                     FED <req> <conf> <ok> [<motivo> [<origem>]]
 *                                               — resultado de uma alimentação
 *                     DENIED <motivo> <refeições_24h> [<origem>]
 *                                               — pedido recusado pelo módulo
 *
 *   motivo do FED     OK | SENSOR (autoteste falhou, contou pelo servo) | VAZIO (nenhum grão caiu)
 *   motivo do DENIED  LIMITE (3 refeições em 24 h) | OCUPADO (outra refeição em andamento)
 *   origem            AGENDA | RECUP | BOTAO | APP | FORCADO
 *
 * Os campos entre colchetes chegaram com o firmware 1.0 do módulo; um módulo
 * anterior que não os mande continua entendido.
 *
 * `FEED` e `CFG` só saem com o módulo conectado (`connected`, linha válida nos
 * últimos 6 s) — quem decide é o `loop()`, que recusa o comando caso contrário.
 */

/** `meals_24h` antes de o módulo dizer quantas refeições houve. */
static const uint8_t FEEDER_MEALS_UNKNOWN = 0xFF;

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
  /** Refeições nas últimas 24 h, contadas pelo módulo; `FEEDER_MEALS_UNKNOWN` antes de ele dizer. */
  uint8_t  meals_24h;
};

void feeder_link_init();

/**
 * Chamado a cada ciclo do loop: amostra a presença no fio, drena o UART, manda
 * o PING periódico (só com o módulo presente) e detecta a desconexão.
 */
void feeder_link_update();

/** O módulo está fisicamente no fio (RX em nível alto)? Seguro de qualquer núcleo. */
bool feeder_link_present();

/**
 * Pede uma alimentação manual. `grains == 0` usa o padrão configurado no
 * módulo; `force` passa por cima do limite de refeições em 24 h — só o app
 * pede isso.
 */
void feeder_link_request_feed(uint8_t grains, bool force);

/** Substitui a agenda inteira do módulo — nunca um campo isolado (ver contrato, `feeder.set_config`). */
void feeder_link_request_config(uint8_t hour1, uint8_t hour2,
                                 uint8_t grains_per_feeding, bool auto_enabled);

/** Cópia do estado conhecido — sempre segura de chamar, mesmo com o módulo ausente. */
FeederLinkState feeder_link_get_state();
