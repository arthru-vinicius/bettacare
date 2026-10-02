#include "protocol.h"

#include <stdio.h>
#include <string.h>

/** Inteiro decimal sem sinal, só dígitos, até `max`. */
static bool _parse_uint(const char* s, uint32_t max, uint32_t& out) {
  if (*s == '\0') return false;
  uint32_t v = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9') return false;
    v = v * 10 + (uint32_t)(*s - '0');
    if (v > max) return false;
  }
  out = v;
  return true;
}

/** Quebra a linha em até `max` tokens separados por espaço. */
static uint8_t _tokens(char* linha, char* toks[], uint8_t max) {
  uint8_t n = 0;
  char* p = linha;
  while (*p) {
    while (*p == ' ') *p++ = '\0';
    if (!*p) break;
    if (n == max) return max + 1;   // tokens demais: lixo
    toks[n++] = p;
    while (*p && *p != ' ') p++;
  }
  return n;
}

bool link_parse(const char* line, LinkRequest& out) {
  char buf[LINK_LINE_MAX];
  size_t len = strlen(line);
  if (len == 0 || len >= sizeof(buf)) return false;
  memcpy(buf, line, len + 1);

  char* t[6];
  uint8_t n = _tokens(buf, t, 6);
  if (n == 0 || n > 6) return false;

  out = {};
  if (strcmp(t[0], "PING") == 0) {
    if (n != 1) return false;
    out.cmd = LinkCmd::PING;
    return true;
  }

  if (strcmp(t[0], "FEED") == 0) {
    // FEED | FEED <n> | FEED FORCE | FEED <n> FORCE
    uint8_t i = 1;
    uint32_t v;
    if (i < n && strcmp(t[i], "FORCE") != 0) {
      if (!_parse_uint(t[i], GRAINS_MAX, v) || v < GRAINS_MIN) return false;
      out.grains = (uint8_t)v;
      i++;
    }
    if (i < n) {
      if (strcmp(t[i], "FORCE") != 0) return false;
      out.force = true;
      i++;
    }
    if (i != n) return false;
    out.cmd = LinkCmd::FEED;
    return true;
  }

  if (strcmp(t[0], "CFG") == 0) {
    if (n != 5) return false;
    uint32_t h1, h2, g, a;
    if (!_parse_uint(t[1], 23, h1) || !_parse_uint(t[2], 23, h2) ||
        !_parse_uint(t[3], GRAINS_MAX, g) || g < GRAINS_MIN || !_parse_uint(t[4], 1, a)) {
      return false;
    }
    out.cmd = LinkCmd::CFG;
    out.cfg = {(uint8_t)h1, (uint8_t)h2, (uint8_t)g, a == 1};
    return true;
  }

  return false;
}

const char* origin_token(Origin o) {
  switch (o) {
    case Origin::AGENDA:  return "AGENDA";
    case Origin::RECUP:   return "RECUP";
    case Origin::BOTAO:   return "BOTAO";
    case Origin::APP:     return "APP";
    case Origin::FORCADO: return "FORCADO";
  }
  return "AGENDA";
}

const char* reason_token(MealReason r) {
  switch (r) {
    case MealReason::OK:     return "OK";
    case MealReason::SENSOR: return "SENSOR";
    case MealReason::VAZIO:  return "VAZIO";
  }
  return "OK";
}

const char* deny_token(DenyReason d) {
  switch (d) {
    case DenyReason::LIMITE:   return "LIMITE";
    case DenyReason::OCUPADO:  return "OCUPADO";
    case DenyReason::CALIBRAR: return "CALIBRAR";
    case DenyReason::PERDIDA:  return "PERDIDA";
  }
  return "LIMITE";
}

/** Grãos no enlace vão até 20 — a faixa do contrato. */
static unsigned _graos(uint8_t g) { return g > GRAINS_MAX ? GRAINS_MAX : g; }

static size_t _ok(int escrito, size_t n) {
  return (escrito > 0 && (size_t)escrito < n) ? (size_t)escrito : 0;
}

size_t link_format_pong(char* buf, size_t n, const FeederConfig& cfg, uint32_t last_feed_age_s,
                        uint8_t req, uint8_t conf, bool ok, uint8_t meals) {
  char idade[12];
  if (last_feed_age_s == UINT32_MAX) {
    strcpy(idade, "-1");
  } else {
    // O teto do contrato (INT32_MAX) — 68 anos, não vai acontecer.
    snprintf(idade, sizeof(idade), "%lu",
             (unsigned long)(last_feed_age_s > 0x7FFFFFFFUL ? 0x7FFFFFFFUL : last_feed_age_s));
  }
  return _ok(snprintf(buf, n, "PONG %u %u %u %u %s %u %u %u %u\n", cfg.hour1, cfg.hour2,
                      _graos(cfg.grains), cfg.auto_enabled ? 1 : 0, idade, _graos(req), _graos(conf),
                      ok ? 1 : 0, meals > 254 ? 254 : meals),
             n);
}

size_t link_format_schedule(char* buf, size_t n, const FeederConfig& cfg) {
  return _ok(snprintf(buf, n, "SCHEDULE %u %u %u %u\n", cfg.hour1, cfg.hour2, _graos(cfg.grains),
                      cfg.auto_enabled ? 1 : 0),
             n);
}

size_t link_format_fed(char* buf, size_t n, const MealResult& r) {
  return _ok(snprintf(buf, n, "FED %u %u %u %s %s\n", _graos(r.requested), _graos(r.confirmed),
                      r.ok ? 1 : 0, reason_token(r.reason), origin_token(r.origin)),
             n);
}

size_t link_format_denied(char* buf, size_t n, DenyReason why, uint8_t meals, Origin origin) {
  return _ok(snprintf(buf, n, "DENIED %s %u %s\n", deny_token(why), meals > 254 ? 254 : meals,
                      origin_token(origin)),
             n);
}
