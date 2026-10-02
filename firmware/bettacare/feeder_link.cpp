#include "feeder_link.h"

#include <driver/gpio.h>
#include <string.h>

#include "config.h"
#include "event_log.h"

/** Sem nenhuma linha reconhecida por este tempo, o módulo conta como desconectado. */
static const uint32_t LINK_TIMEOUT_MS = 6000;
/** Intervalo entre PINGs, com o módulo presente no fio. */
static const uint32_t PING_INTERVAL_MS = 2000;
/** Linha maior que isto nunca deveria acontecer com este protocolo — descartada. */
static const uint8_t LINE_BUF_LEN = 96;

/**
 * Amostras seguidas (uma por ciclo do loop, ~200 ms) para mudar a presença.
 *
 * Ausente: cinco amostras em nível baixo, ~1 s. Com o módulo ligado, o fio só
 * desce durante os bits de um byte — ~104 µs cada a 9600 baud, numa linha de
 * ~25 ms a cada 2 s —, e cinco amostras de ciclos diferentes nunca caem todas
 * dentro de bits.
 * Presente: duas em nível alto, para um pico de ruído isolado não bastar.
 */
static const uint8_t ABSENT_SAMPLES  = 5;
static const uint8_t PRESENT_SAMPLES = 2;

static FeederLinkState _state = {};
/** Evita emitir "desconectou" no boot, quando o módulo nunca chegou a responder. */
static bool     _ever_connected = false;
static uint32_t _last_rx_ms = 0;
static uint32_t _last_ping_ms = 0;
/** `millis()` de quando `_state.last_feed_age_s` foi medido pela última vez. */
static uint32_t _last_feed_measured_ms = 0;

/**
 * O módulo está fisicamente no fio? Lido também pelo `/status` local, de outro
 * núcleo — um `bool` é lido e escrito numa instrução.
 */
static volatile bool _present = false;
static uint8_t       _high_samples = 0;
static uint8_t       _low_samples = 0;

static char    _line_buf[LINE_BUF_LEN];
static uint8_t _line_len = 0;

void feeder_link_init() {
  Serial2.begin(FEEDER_LINK_BAUD, SERIAL_8N1, PIN_FEEDER_RX, PIN_FEEDER_TX);
  /**
   * Detecção física do módulo pelo próprio fio de dados, sem fio a mais no
   * conector. A UART repousa em nível alto: com o módulo ligado, o GPIO16
   * fica em alto; com o conector vazio, o pull-down o segura em baixo. Sem
   * módulo no fio, o principal não manda nada por ele (v1.1.1).
   *
   * Até a 2.0.0 era pull-up: segurava o fio vazio quieto (ele vira antena ao
   * lado do PWM do GPIO17), mas no mesmo nível de um módulo ocioso — e o PING
   * saía a cada 2 s para ninguém. O pull-down faz as duas coisas.
   * `gpio_*` e não `pinMode`: este tiraria o pino do UART.
   */
  gpio_pullup_dis((gpio_num_t)PIN_FEEDER_RX);
  gpio_pulldown_en((gpio_num_t)PIN_FEEDER_RX);
  _state = {};
  _state.last_feed_age_s = UINT32_MAX;
  _ever_connected = false;
  _last_rx_ms = 0;
  _last_ping_ms = 0;
  _last_feed_measured_ms = 0;
  _present = false;
  _high_samples = 0;
  _low_samples = 0;
  _line_len = 0;
}

static void _sample_presence() {
  if (gpio_get_level((gpio_num_t)PIN_FEEDER_RX)) {
    _low_samples = 0;
    if (!_present && ++_high_samples >= PRESENT_SAMPLES) _present = true;
  } else {
    _high_samples = 0;
    if (_present && ++_low_samples >= ABSENT_SAMPLES) _present = false;
  }
}

/**
 * Um token da linha, separado por espaço. Falso se não houver mais nenhum — ou
 * se ele não couber em `out`: cortado ao meio, o resto viraria o campo
 * seguinte ("00000001" lido como hora 0 seguida de hora 1).
 */
static bool _next_token(char*& p, char* out, uint8_t out_len) {
  while (*p == ' ') p++;
  if (*p == '\0') return false;
  uint8_t i = 0;
  while (*p != ' ' && *p != '\0') {
    if (i >= out_len - 1) return false;
    out[i++] = *p++;
  }
  out[i] = '\0';
  return true;
}

static void _mark_connected() {
  _last_rx_ms = millis();
  // Quem mandou linha válida está no fio, mesmo antes das duas amostras.
  _present = true;
  _low_samples = 0;
  if (_state.connected) return;
  _state.connected = true;
  // Só registra "conectou" se antes já tinha desconectado (ou é a primeira
  // vez de verdade) — na primeira conexão do boot isto ainda não emite nada,
  // porque `_ever_connected` só vira `true` logo abaixo.
  if (_ever_connected) {
    event_log(SEV_INFO, COMP_FEEDER, "feeder.module_connected",
              "Modulo do alimentador respondeu");
  }
  _ever_connected = true;
}

/**
 * Inteiro decimal sem sinal, só dígitos, até `max`. Falso para qualquer outra
 * coisa.
 *
 * O enlace não tem checksum, e o RX (GPIO16) corre ao lado do PWM de 25 kHz da
 * ventoinha (GPIO17): um dígito trocado por ruído não pode virar "alimenta às
 * 47h" na interface. `atoi` aceitava lixo como zero e qualquer número como
 * válido; aqui um campo fora da faixa do contrato descarta a linha inteira, e
 * o próximo PONG, 2 s depois, traz o valor certo.
 */
static bool _parse_uint(const char* s, uint32_t max, uint32_t& out) {
  if (*s == '\0') return false;
  uint64_t v = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9') return false;
    v = v * 10 + (uint64_t)(*s - '0');
    if (v > max) return false;
  }
  out = (uint32_t)v;
  return true;
}

/** Os quatro campos da agenda, nas faixas do contrato (`feeder.set_config`). */
struct FeederSchedule {
  uint8_t hour1, hour2, grains;
  bool    auto_enabled;
};

static bool _parse_schedule(char*& p, FeederSchedule& out) {
  char h1[8], h2[8], g[8], a[8];
  if (!_next_token(p, h1, sizeof(h1)) || !_next_token(p, h2, sizeof(h2)) ||
      !_next_token(p, g, sizeof(g)) || !_next_token(p, a, sizeof(a))) {
    return false;
  }
  uint32_t v1, v2, vg, va;
  if (!_parse_uint(h1, 23, v1) || !_parse_uint(h2, 23, v2) ||
      !_parse_uint(g, 20, vg) || vg < 1 || !_parse_uint(a, 1, va)) {
    return false;
  }
  out.hour1 = (uint8_t)v1;
  out.hour2 = (uint8_t)v2;
  out.grains = (uint8_t)vg;
  out.auto_enabled = va != 0;
  return true;
}

static void _apply_schedule(const FeederSchedule& s) {
  _state.hour1 = s.hour1;
  _state.hour2 = s.hour2;
  _state.grains_per_feeding = s.grains;
  _state.auto_enabled = s.auto_enabled;
}

/** `<req> <conf> <ok>` do resultado de uma alimentação. */
static bool _parse_feed_result(char*& p, uint8_t& req, uint8_t& conf, bool& ok) {
  char r[8], c[8], o[8];
  if (!_next_token(p, r, sizeof(r)) || !_next_token(p, c, sizeof(c)) ||
      !_next_token(p, o, sizeof(o))) {
    return false;
  }
  uint32_t vr, vc, vo;
  if (!_parse_uint(r, 20, vr) || !_parse_uint(c, 20, vc) || !_parse_uint(o, 1, vo)) {
    return false;
  }
  req = (uint8_t)vr;
  conf = (uint8_t)vc;
  ok = vo != 0;
  return true;
}

static void _handle_line(char* line) {
  char tok[24];
  char* p = line;
  if (!_next_token(p, tok, sizeof(tok))) return;

  // Qualquer linha **reconhecível** prova que o módulo está vivo — não só o
  // PONG. Linha desconhecida não prova nada: antes, ruído no RX com o
  // conector vazio marcava o módulo como conectado e, 6 s depois, gerava um
  // "desconectou" que nunca aconteceu (UPGRADE/07).
  bool reconhecida = strcmp(tok, "PONG") == 0 || strcmp(tok, "SCHEDULE") == 0 ||
                     strcmp(tok, "FED") == 0;
  if (!reconhecida) return;
  _mark_connected();

  if (strcmp(tok, "PONG") == 0) {
    FeederSchedule agenda;
    char age[12];
    uint8_t req, conf;
    bool ok;
    if (!_parse_schedule(p, agenda) || !_next_token(p, age, sizeof(age)) ||
        !_parse_feed_result(p, req, conf, ok)) {
      return;
    }
    // `-1` é o "nunca alimentou" do módulo; o teto é o do contrato (INT32_MAX).
    uint32_t idade = UINT32_MAX;
    if (strcmp(age, "-1") != 0 && !_parse_uint(age, INT32_MAX, idade)) return;

    _apply_schedule(agenda);
    _state.last_feed_age_s = idade;
    _last_feed_measured_ms = millis();
    _state.last_feed_requested = req;
    _state.last_feed_confirmed = conf;
    _state.last_feed_ok = ok;
    return;
  }

  if (strcmp(tok, "SCHEDULE") == 0) {
    // O módulo empurra isto sozinho a cada reconexão — a NVS dele é a fonte
    // de verdade da agenda, não o servidor (ver pinagem-alimentador-modulo.md).
    FeederSchedule agenda;
    if (!_parse_schedule(p, agenda)) return;
    _apply_schedule(agenda);
    event_log(SEV_INFO, COMP_FEEDER, "feeder.config_applied",
              "Agenda do modulo: %02u:00 e %02u:00, %u graos, automatico %s",
              (unsigned)_state.hour1, (unsigned)_state.hour2,
              (unsigned)_state.grains_per_feeding,
              _state.auto_enabled ? "ligado" : "desligado");
    return;
  }

  if (strcmp(tok, "FED") == 0) {
    uint8_t req, conf;
    bool ok;
    if (!_parse_feed_result(p, req, conf, ok)) return;
    _state.last_feed_requested = req;
    _state.last_feed_confirmed = conf;
    _state.last_feed_ok = ok;
    _state.last_feed_age_s = 0;
    _last_feed_measured_ms = millis();

    if (_state.last_feed_confirmed < _state.last_feed_requested) {
      event_log(SEV_WARN, COMP_FEEDER, "feeder.fed_incomplete",
                "Alimentacao incompleta: %u de %u graos confirmados",
                (unsigned)_state.last_feed_confirmed,
                (unsigned)_state.last_feed_requested);
    } else {
      event_log(SEV_INFO, COMP_FEEDER, "feeder.fed_ok",
                "Alimentacao concluida: %u graos confirmados",
                (unsigned)_state.last_feed_confirmed);
    }
    return;
  }
}

void feeder_link_update() {
  _sample_presence();

  while (Serial2.available()) {
    char c = (char)Serial2.read();
    // O 0x00 que a UART entrega quando o fio desce e fica em baixo (o
    // "break" de desplugar o cabo) não é texto; no começo de uma linha, faria
    // o `PONG` seguinte ser descartado.
    if (c == '\0') continue;
    if (c == '\n' || c == '\r') {
      if (_line_len > 0) {
        _line_buf[_line_len] = '\0';
        _handle_line(_line_buf);
        _line_len = 0;
      }
      continue;
    }
    // Byte de ruído além do teto: descarta em silêncio até o fim da linha
    // em vez de travar o link — não há nenhuma mensagem legítima deste
    // protocolo perto desse tamanho.
    if (_line_len < LINE_BUF_LEN - 1) _line_buf[_line_len++] = c;
  }

  uint32_t agora = millis();

  // Cabo desligado ou módulo mudo: o estado NORMAL de um módulo que não fica
  // ligado o tempo todo — nunca evento acima de `info`. Sem o cabo, cai na
  // hora; com ele, depois do timeout.
  if (_state.connected && (!_present || agora - _last_rx_ms > LINK_TIMEOUT_MS)) {
    _state.connected = false;
    event_log(SEV_INFO, COMP_FEEDER, "feeder.module_disconnected",
              _present ? "Sem resposta do modulo do alimentador"
                       : "Modulo do alimentador saiu do cabo");
  }

  // Só fala com quem está no fio.
  if (_present && agora - _last_ping_ms >= PING_INTERVAL_MS) {
    _last_ping_ms = agora;
    Serial2.print("PING\n");
  }
}

bool feeder_link_present() { return _present; }

FeederLinkState feeder_link_get_state() {
  FeederLinkState s = _state;
  // Envelhece a idade da última alimentação em tempo real entre uma medição
  // e a próxima — sem isto a interface mostraria "há 3s" congelado até o
  // próximo PONG, mesmo minutos depois.
  if (s.last_feed_age_s != UINT32_MAX && _last_feed_measured_ms != 0) {
    s.last_feed_age_s += (millis() - _last_feed_measured_ms) / 1000;
  }
  return s;
}

void feeder_link_request_feed(uint8_t grains) {
  if (grains == 0) {
    Serial2.print("FEED\n");
  } else {
    Serial2.printf("FEED %u\n", (unsigned)grains);
  }
}

void feeder_link_request_config(uint8_t hour1, uint8_t hour2,
                                 uint8_t grains_per_feeding, bool auto_enabled) {
  Serial2.printf("CFG %u %u %u %u\n", (unsigned)hour1, (unsigned)hour2,
                  (unsigned)grains_per_feeding, auto_enabled ? 1 : 0);
}
