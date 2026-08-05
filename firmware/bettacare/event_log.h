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
 * Copia até `max` eventos para `out` e os remove do buffer.
 *
 * Só a task de rede chama isto, e só depois de o POST ter sido aceito — se
 * drenasse antes, uma falha de rede perderia os eventos justamente no momento
 * em que eles importam.
 */
uint8_t event_log_drain(LogEvent* out, uint8_t max);

/** Quantos eventos aguardam envio. */
uint8_t event_log_pending();

/**
 * Verdadeiro se algum evento foi descartado por estouro do buffer desde a
 * última consulta. A task de rede converte isso num `system.event_overflow`.
 */
bool event_log_take_overflow_flag();
