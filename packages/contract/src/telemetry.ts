import { z } from "zod";
import { commandAckSchema, pendingCommandSchema } from "./commands.js";
import { deviceConfigSchema } from "./config.js";
import { deviceEventSchema, MAX_EVENTS_PER_POST } from "./events.js";
import { deviceHealthReportSchema } from "./health.js";
import {
  deviceIdSchema,
  INT32_MAX,
  isoInstantSchema,
  timeOfDaySchema,
  UINT32_MAX,
} from "./primitives.js";

/**
 * `POST /api/v1/telemetry` — o único endpoint que o ESP32 usa.
 *
 * Um request serve os dois sentidos: o corpo leva o estado do aquário, a
 * resposta traz os comandos pendentes. O servidor **nunca inicia conexão**, e é
 * por isso que o IP dinâmico do dispositivo deixa de ser um problema.
 */

export const TELEMETRY_PATH = "/api/v1/telemetry";
export const INGEST_TOKEN_HEADER = "x-api-token";
export const ACCESS_EMAIL_HEADER = "cf-access-authenticated-user-email";

// ── Blocos de estado ──────────────────────────────────────────────────────

export const lightStateSchema = z.object({
  on: z.boolean(),
  /**
   * Quem determinou o estado atual. Distingue "eu apaguei" de "a automação
   * apagou" — e sem isso o histórico de luz fica indecifrável.
   */
  source: z.enum(["schedule", "manual", "button", "command", "boot"]),
});
export type LightState = z.infer<typeof lightStateSchema>;

export const temperatureStateSchema = z.object({
  /** Nulo quando o sensor não respondeu. Não usar 0 nem -127 como sentinela. */
  celsius: z.number().min(-55).max(125).nullable(),
  /** O sensor foi encontrado no barramento 1-Wire? */
  available: z.boolean(),
  /** A última leitura passou no CRC? */
  valid: z.boolean(),
  /**
   * Idade da leitura em ms — é o que define `temp.stale`.
   *
   * **Nulo quando nunca houve leitura válida.** A idade de algo que não
   * aconteceu não é um número grande, é ausência — e tratá-la como número foi
   * o bug que derrubava o ingest: o firmware mandava `UINT32_MAX` e a coluna
   * `integer` estourava.
   */
  age_ms: z.int().min(0).max(UINT32_MAX).nullable(),
});
export type TemperatureState = z.infer<typeof temperatureStateSchema>;

export const fanStateSchema = z.object({
  on: z.boolean(),
  speed_percent: z.int().min(0).max(100),
  /**
   * Rotação medida pelo tacômetro. É a **única realimentação real** do sistema:
   * `speed_percent > 0` com `rpm == 0` significa que a ventoinha não gira.
   */
  rpm: z.int().min(0).max(20000),
  mode: z.enum(["auto", "manual", "manual_off", "failsafe"]),
});
export type FanState = z.infer<typeof fanStateSchema>;

export const rtcStateSchema = z.object({
  available: z.boolean(),
  time: timeOfDaySchema.nullable(),
  /** O DS3231 acusou perda de energia — bateria no fim. */
  lost_power: z.boolean().default(false),
});
export type RtcState = z.infer<typeof rtcStateSchema>;

export const wifiStateSchema = z.object({
  rssi: z.int().min(-120).max(0),
  ip: z.ipv4().nullable(),
  /** Reconexões desde o boot. Subindo depressa = link instável. */
  reconnects: z.int().min(0).max(INT32_MAX).default(0),
});
export type WifiState = z.infer<typeof wifiStateSchema>;

// ── Requisição ────────────────────────────────────────────────────────────

export const telemetryRequestSchema = z.object({
  device_id: deviceIdSchema,
  /** Contador em NVS, incrementado a cada boot. Detecta reinícios. */
  boot_id: z.int().min(0).max(INT32_MAX),
  /** Monotônico dentro de um boot. É o que dá idempotência ao ingest. */
  seq: z.int().min(0).max(INT32_MAX),
  /** Vem de `millis()`: passa de `INT32_MAX` com ~25 dias de uptime. */
  uptime_ms: z.int().min(0).max(UINT32_MAX),
  /** O que o RTC do dispositivo acha que é. Nulo se o módulo sumiu. */
  device_time: isoInstantSchema.nullable().optional(),
  fw_version: z.string().max(32).optional(),
  config_version: z.int().min(0),

  light: lightStateSchema,
  temperature: temperatureStateSchema,
  fan: fanStateSchema,
  rtc: rtcStateSchema,
  wifi: wifiStateSchema,

  /** A opinião do firmware sobre a própria saúde. Opcional. */
  health: deviceHealthReportSchema.optional(),
  /** Confirmações dos comandos entregues no POST anterior. */
  ack: z.array(commandAckSchema).max(16).default([]),
  /** Eventos acumulados desde o último POST. */
  events: z.array(deviceEventSchema).max(MAX_EVENTS_PER_POST).default([]),
});
export type TelemetryRequest = z.infer<typeof telemetryRequestSchema>;

// ── Resposta ──────────────────────────────────────────────────────────────

/**
 * Corpo pequeno e de tamanho previsível — o contrato do homelab exige, e o
 * ESP32 agradece. No caso normal fica em torno de 100 bytes, porque o bloco
 * `config` só viaja quando o `config_version` do dispositivo está atrasado.
 */
export const telemetryResponseSchema = z.object({
  ok: z.literal(true),
  server_time: isoInstantSchema,
  config_version: z.int().min(0),
  config: deviceConfigSchema.optional(),
  commands: z.array(pendingCommandSchema).default([]),
});
export type TelemetryResponse = z.infer<typeof telemetryResponseSchema>;

/** Resposta de erro. Curta de propósito: o ESP32 só precisa saber que falhou. */
export const telemetryErrorSchema = z.object({
  ok: z.literal(false),
  error: z.string().max(64),
});
export type TelemetryError = z.infer<typeof telemetryErrorSchema>;
