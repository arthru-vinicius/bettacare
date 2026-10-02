#pragma once
#include <stdint.h>

/**
 * Comportamento do alimentador — toda decisão de projeto num lugar só, e sem
 * nada de hardware: os módulos de lógica pura (agenda, protocolo, gestos,
 * doseador) e os testes de host incluem só isto. Pinos e credenciais ficam no
 * `config.h`.
 *
 * Os números de segurança do peixe saíram de decisões do Arthur (2026-10-02):
 * no máximo 3 refeições em 24 h, recuperar uma refeição perdida até 4 h de
 * atraso, e alimentar contando pelo servo se o sensor falhar.
 */

// ── Agenda de fábrica ────────────────────────────────────────────────────────
static const uint8_t DEFAULT_HOUR1  = 5;
static const uint8_t DEFAULT_HOUR2  = 17;
static const uint8_t DEFAULT_GRAINS = 5;
static const bool    DEFAULT_AUTO   = true;
static const uint8_t GRAINS_MIN = 1;
static const uint8_t GRAINS_MAX = 20;

// ── Segurança do peixe ───────────────────────────────────────────────────────
/**
 * Refeições em 24 h, somando agenda, botão e app — o `FEEDER_MEALS_PER_24H`
 * do contrato. Além disso o módulo recusa; só o app passa por cima (FORCE).
 */
static const uint8_t  MEALS_PER_24H = 3;
/** Até este atraso depois do horário, é a refeição normal da agenda. */
static const uint32_t ON_TIME_S = 60UL * 60;
/** Até este atraso, uma refeição perdida é recuperada... */
static const uint32_t RECOVERY_WINDOW_S = 4UL * 3600;
/**
 * ...desde que a próxima da agenda esteja a mais disto, e a última refeição
 * (de qualquer origem) também: a recuperada nunca cola em outra.
 */
static const uint32_t RECOVERY_MIN_GAP_S = 4UL * 3600;
/** Refeições que o histórico guarda — sobra para contar as de 24 h. */
static const uint8_t  MEALS_HISTORY = 8;

// ── Doseador ─────────────────────────────────────────────────────────────────
/** Movimentos seguidos sem grão antes de parar: reservatório vazio ou slide travado. */
static const uint8_t  EMPTY_TRIES = 3;
/** Teto de movimentos numa refeição: grãos × 3 + 3. */
static const uint8_t  DUMPS_PER_GRAIN = 3;
/** Depois de o servo chegar: o slide assenta e o bolso enche. */
static const uint16_t SERVO_SETTLE_MS = 250;
/** Quanto o grão leva, no máximo, para cruzar o feixe depois do despejo. */
static const uint16_t DETECT_WINDOW_MS = 700;
/** Um grão, uma contagem: bordas mais próximas que isto são o mesmo grão. */
static const uint16_t IR_DEBOUNCE_MS = 15;
/** Teto de uma refeição inteira, aconteça o que acontecer. */
static const uint32_t MEAL_MAX_MS = 90000UL;
/** O LED IR acender ou apagar e o LM393 assentar. */
static const uint16_t SELFTEST_SETTLE_MS = 30;

// ── Motores ──────────────────────────────────────────────────────────────────
/** O 1027 é motor de 3 V: 60% de 5 V. Sobretensão gasta a escova. */
static const uint8_t  M1_DUTY_PCT = 60;
static const uint16_t M1_PRE_MS = 300;           // antes do primeiro grão
static const uint16_t M1_RETRY_MS = 500;         // antes de repetir um movimento vazio
static const uint16_t M1_DAILY_MS = 2000;        // o pulso diário, mesmo sem refeição
static const uint32_t M1_DAILY_INTERVAL_S = 24UL * 3600;
static const uint16_t M1_MAX_MS = 3000;          // teto absoluto de um pulso
/** O aviso ao peixe: rampa suave, nunca um tranco na água. */
static const uint8_t  M2_DUTY_PCT = 55;
static const uint16_t M2_RAMP_MS = 1000;
static const uint16_t M2_HOLD_MS = 2000;
static const uint16_t M2_MAX_MS = 6000;          // teto absoluto de um aviso

// ── Servo (MG90S) ────────────────────────────────────────────────────────────
/** Só valem até a primeira calibração — antes dela, nenhuma refeição sai. */
static const uint8_t  SERVO_DEFAULT_REST = 40;
static const uint8_t  SERVO_DEFAULT_DUMP = 100;
static const uint16_t SERVO_US_MIN = 500;        // 0°
static const uint16_t SERVO_US_MAX = 2400;       // 180°
/** Velocidade: 5° a cada 10 ms — sem tranco no slide nem nas engrenagens. */
static const uint8_t  SERVO_STEP_DEG = 5;
static const uint16_t SERVO_STEP_MS = 10;
/** Passo do ajuste fino pelo botão, na calibração. */
static const uint8_t  CAL_STEP_DEG = 2;
/** Curso mínimo entre repouso e despejo: menos que isto não troca o bolso de lado. */
static const uint8_t  CAL_MIN_TRAVEL_DEG = 10;

// ── Interface ────────────────────────────────────────────────────────────────
static const uint16_t SCREEN_ON_MS = 5000;
static const uint16_t PROGRAM_IDLE_MS = 20000;
static const uint32_t CALIBRATE_IDLE_MS = 120000UL;
static const uint16_t TAP_WINDOW_MS = 400;
static const uint16_t LONG_PRESS_MS = 2000;
static const uint8_t  BUTTON_SAMPLE_MS = 2;
static const uint8_t  BUTTON_STABLE_MS = 30;

// ── Enlace com o principal ───────────────────────────────────────────────────
/** Sem PING por este tempo, o principal conta como fora do fio. */
static const uint32_t LINK_TIMEOUT_MS = 6000;
/** Linha maior que isto nunca deveria acontecer neste protocolo — descartada. */
static const uint8_t  LINK_LINE_MAX = 96;
/** Avisos guardados enquanto o principal está fora do fio. */
static const uint8_t  PENDING_LINES = 4;
