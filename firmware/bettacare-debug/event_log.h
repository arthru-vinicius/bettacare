#pragma once

#include <Arduino.h>

/**
 * Eventos estruturados.
 *
 * Substitui o `log_manager` antigo, que guardava 30 strings livres num buffer
 * circular em RAM e as publicava por MQTT. Aquilo era texto: não dava para
 * filtrar, agregar nem alertar em cima.
 *
 * Agora cada evento tem severidade, componente e um **código estável**, que é
 * o que permite perguntar "quantas vezes o DS18B20 caiu este mês?" sem
 * comparar strings. A mensagem é para humano e pode mudar de redação sem
 * quebrar nada.
 *
 * Thread-safe: o controle local (núcleo 1) escreve, a task de rede (núcleo 0)
 * drena. Toda a API pega o mutex interno.
 */

enum EventSeverity : uint8_t {
  SEV_DEBUG = 0,
  SEV_INFO,
  SEV_WARN,
  SEV_ERROR,
  SEV_FATAL,
};

/** Precisa casar com o enum `component` do contrato. */
enum EventComponent : uint8_t {
  COMP_SYSTEM = 0,
  COMP_WIFI,
  COMP_API,
  COMP_RTC,
  COMP_TEMP,
  COMP_FAN,
  COMP_LIGHT,
  COMP_BUTTON,
  COMP_POT,
  COMP_NVS,
  COMP_OTA,
  /** Módulo opcional de alimentação de precisão — ver `feeder_link.h`. Sempre por último: o valor numérico nunca viaja, só o nome. */
  COMP_FEEDER,
};

static const uint8_t EVENT_CODE_LEN = 40;
static const uint8_t EVENT_MSG_LEN  = 120;
/** Teto por POST, definido pelo contrato (`MAX_EVENTS_PER_POST`). */
static const uint8_t EVENT_BUFFER_SIZE = 24;

struct LogEvent {
  EventSeverity  sev;
  EventComponent comp;
  char           code[EVENT_CODE_LEN];
  char           msg[EVENT_MSG_LEN];
  /** Hora local do RTC no formato "HH:MM", ou "--:--" se o módulo sumiu. */
  char           time[6];
  uint16_t       repeat_count;
};

void event_log_init();

/**
 * Registra um evento.
 *
 * Repetições do mesmo `(comp, code)` **enquanto ainda for a entrada mais
 * recente** são colapsadas incrementando `repeat_count`, em vez de gerar N
 * linhas iguais. Um sensor oscilando não pode inundar o banco.
 */
void event_log(EventSeverity sev, EventComponent comp, const char* code,
               const char* fmt, ...);

/** Nomes usados na serialização — precisam casar com o contrato em Zod. */
const char* event_severity_name(EventSeverity sev);
const char* event_component_name(EventComponent comp);

/**
 * Copia até `max` eventos para `out` **sem removê-los**.
 *
 * Substitui o antigo `event_log_drain()`, que removia na hora de montar o
 * corpo — antes de saber se o POST daria certo. O resultado é que uma falha de
 * rede apagava os eventos justamente no momento em que eles importam.
 *
 * Agora a remoção é decidida depois, pelo desfecho: ver
 * `event_log_confirm_sent()` e `event_log_release_sent()`.
 */
uint8_t event_log_peek(LogEvent* out, uint8_t max);

/** POST aceito: remove os `n` mais antigos, que acabaram de sair. */
void event_log_confirm_sent(uint8_t n);

/**
 * POST falhou: descarta os `n` mais antigos **exceto** `error` e `fatal`, que
 * ficam para a próxima tentativa.
 *
 * O meio-termo é deliberado. Reinserir tudo criaria o laço que a decisão
 * original evitava — falha de rede gera evento, que engorda o próximo corpo,
 * que tem mais chance de falhar. Mas descartar tudo joga fora exatamente o
 * diagnóstico que explica a falha. Retendo só o que é grave, o volume
 * retido é minúsculo (erros são raros) e a informação que sobrevive é a que
 * vale.
 */
void event_log_release_sent(uint8_t n);

/** Quantos eventos aguardam envio. */
uint8_t event_log_pending();

/**
 * Verdadeiro se algum evento foi descartado por estouro do buffer desde a
 * última consulta. A task de rede converte isso num `system.event_overflow`.
 */
bool event_log_take_overflow_flag();

/**
 * Total de eventos perdidos por estouro desde o boot.
 *
 * Vai no bloco `diag` da telemetria: perder evento é perder diagnóstico, e o
 * servidor precisa saber que a história que ele tem está incompleta.
 */
uint16_t event_log_dropped_count();
