#pragma once
#include <stddef.h>

#include "types.h"

/**
 * O lado do módulo do enlace com o ESP32 principal — uma linha ASCII por
 * mensagem, 9600 baud. Referência completa em
 * `firmware/bettacare/feeder_link.h`:
 *
 *   principal → módulo   PING
 *                         FEED [grãos] [FORCE]
 *                         CFG <h1> <h2> <grãos> <0|1>
 *
 *   módulo → principal   PONG <h1> <h2> <grãos> <auto> <idade_s> <req> <conf> <ok> <refeições_24h>
 *                         SCHEDULE <h1> <h2> <grãos> <auto>
 *                         FED <req> <conf> <ok> <motivo> <origem>
 *                         DENIED <motivo> <refeições_24h> <origem>
 *
 * Lógica pura: interpreta e monta texto, sem tocar no UART.
 */

enum class LinkCmd : uint8_t { NONE, PING, FEED, CFG };

struct LinkRequest {
  LinkCmd      cmd;
  uint8_t      grains;   // FEED: 0 = o da agenda
  bool         force;    // FEED: passa por cima do limite de 24 h
  FeederConfig cfg;      // CFG
};

/**
 * Interpreta uma linha do principal (sem o "\n"). Falso para lixo ou campo
 * fora da faixa: o enlace não tem checksum, e um dígito trocado por ruído não
 * pode virar "alimenta às 47h" — a linha inteira é descartada.
 */
bool link_parse(const char* line, LinkRequest& out);

/** `idade_s`: segundos desde a última alimentação; `UINT32_MAX` = nunca. */
size_t link_format_pong(char* buf, size_t n, const FeederConfig& cfg, uint32_t last_feed_age_s,
                        uint8_t req, uint8_t conf, bool ok, uint8_t meals);
size_t link_format_schedule(char* buf, size_t n, const FeederConfig& cfg);
size_t link_format_fed(char* buf, size_t n, const MealResult& r);
size_t link_format_denied(char* buf, size_t n, DenyReason why, uint8_t meals, Origin origin);

const char* origin_token(Origin o);
const char* reason_token(MealReason r);
const char* deny_token(DenyReason d);
