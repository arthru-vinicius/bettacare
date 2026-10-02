#pragma once

#include <Arduino.h>

/**
 * Instrumentação de investigação — só existe no firmware de debug.
 *
 * Nasceu para achar o relé que mudava de estado sem comando (UPGRADE/07): um
 * anel de eventos com carimbo de tempo, escrito de três contextos (ISR do
 * botão, loop de controle, task de rede) e lido pelo endpoint `/debug`, mais
 * chaves de runtime que ligam e desligam subsistemas sem regravar o firmware —
 * para isolar a causa por experimento controlado, um subsistema de cada vez.
 *
 * Depois ganhou ganchos que **injetam falhas** (I²C do DS3231, valor de reset
 * do DS18B20) para exercitar ao vivo os caminhos de erro que, no uso normal,
 * só apareceriam com defeito de verdade.
 */
enum DbgKind : uint8_t {
  DBG_BTN_EDGE = 1,  // ISR de borda no GPIO18; a = 1 se o pino já estava HIGH na entrada da ISR
  DBG_BTN_TOGGLE,    // light_set() vindo do caminho do botão; a = estado novo
  DBG_SCHED_TOGGLE,  // light_set() vindo da automação de horário; a = estado novo
  DBG_CMD_TOGGLE,    // light_set() vindo de comando do servidor; a = estado novo
  DBG_OW_REQUEST,    // DS18B20: requestTemperatures()
  DBG_OW_READ,       // DS18B20: getTempCByIndex()
  DBG_OW_PROBE,      // DS18B20: begin() + busca no barramento; a = sensores achados
  DBG_I2C_READ,      // DS3231: leitura da hora; a = 0 ok, 1 erro de I²C, 2 inválida; dur = minutos lidos
  DBG_FEEDER_PING,   // UART2: PING para o módulo do alimentador
  DBG_POST,          // POST de telemetria; a = status HTTP
  DBG_SSR_MISMATCH,  // GPIO23 lido diferente do último valor que light_set() escreveu
  DBG_BTN_PRESS,     // pressão confirmada pelo debounce
};

/** Chamável de ISR (a definição fica em IRAM). */
void dbg_mark_isr(DbgKind kind, uint16_t a);
void dbg_mark(DbgKind kind, uint16_t a = 0, uint16_t dur_ms = 0);

extern volatile uint32_t dbg_btn_edges;
extern volatile uint32_t dbg_btn_edges_high;
extern volatile uint32_t dbg_btn_toggles;
extern volatile uint32_t dbg_sched_toggles;
extern volatile uint32_t dbg_cmd_toggles;
extern volatile uint32_t dbg_ssr_mismatches;
extern volatile int      dbg_last_http_status;
extern volatile uint32_t dbg_http_400;
extern volatile int      dbg_wdt_result;   // esp_err_t da configuração do watchdog da task de rede
extern volatile uint32_t dbg_loop_stack_hwm;   // menor folga da pilha do loop, em bytes

// ── Chaves de experimento ────────────────────────────────────────────────────
extern volatile bool     dbg_temp_enabled;
extern volatile uint32_t dbg_temp_interval_ms;
extern volatile bool     dbg_rtc_enabled;
extern volatile bool     dbg_feeder_enabled;

// ── Injeção de falhas (cada contador é consumido uma leitura por vez) ────────
extern volatile uint32_t dbg_rtc_fail_reads;    // próximas N leituras do DS3231 falham no I²C
extern volatile uint32_t dbg_rtc_bad_reads;     // próximas N voltam com bytes inválidos (0x40:0x08)
extern volatile uint32_t dbg_rtc_offset_reads;  // próximas N leituras válidas saem deslocadas...
extern volatile int32_t  dbg_rtc_offset_min;    // ...por estes minutos (hora válida, porém errada)
extern volatile uint32_t dbg_temp_fake_85;      // próximas N leituras do DS18B20 voltam 85,0 °C
extern volatile bool     dbg_diag_request;      // roda o autodiagnóstico na próxima volta do loop
extern volatile bool     dbg_hang_loop;         // trava o loop de propósito: o watchdog tem de reiniciar o chip em 60 s

/**
 * Espelho dos últimos eventos de `event_log()`, para ver no `/debug` o que o
 * firmware registrou sem depender do servidor (nem do cabo USB).
 */
void dbg_note_event(const char* code, const char* msg);

/** JSON com contadores, chaves, memória, os últimos eventos e o anel inteiro (mais antigo primeiro). */
String dbg_dump_json();
