import {
  AQUARIUM_COMPONENTS,
  COMPONENTS,
  COMMAND_ACTION_NAMES,
  HEALTH_STATUSES,
  SEVERITIES,
} from "@bettacare/contract";
import { sql } from "drizzle-orm";
import {
  bigint,
  boolean,
  index,
  integer,
  jsonb,
  pgEnum,
  pgTable,
  primaryKey,
  real,
  smallint,
  text,
  timestamp,
} from "drizzle-orm/pg-core";

/**
 * Schema do BettaCare.
 *
 * Duas convenções valem para o arquivo inteiro:
 *
 * 1. **Todo timestamp é `timestamptz` em UTC.** O fuso só aparece na
 *    apresentação, vindo de `TZ_DISPLAY`. Depender do fuso do processo é fonte
 *    de bug, e o contrato do homelab exige UTC.
 * 2. **`telemetry` e `events` são particionadas por mês.** O Drizzle não
 *    exprime `PARTITION BY`, então as definições aqui descrevem a *forma* das
 *    tabelas e a cláusula de particionamento é aplicada por SQL na migration.
 *    Ver `docs/arquitetura-observabilidade.md`, seção 5.
 */

// ── Enums ─────────────────────────────────────────────────────────────────
// Derivados do contrato: acrescentar um componente lá quebra o build aqui,
// que é exatamente o que queremos.

export const componentEnum = pgEnum("component", COMPONENTS);
export const severityEnum = pgEnum("severity", SEVERITIES);
export const healthStatusEnum = pgEnum("health_status", HEALTH_STATUSES);
export const eventSourceEnum = pgEnum("event_source", ["device", "server"]);
export const commandStatusEnum = pgEnum("command_status", [
  "queued",
  "sent",
  "acked",
  "rejected",
  "expired",
  "superseded",
]);
export const commandTargetEnum = pgEnum("command_target", [
  "light",
  "fan",
  "config",
  "device",
]);
export const commandActionEnum = pgEnum("command_action", COMMAND_ACTION_NAMES);
export const lightSourceEnum = pgEnum("light_source", [
  "schedule",
  "manual",
  "button",
  "command",
  "boot",
]);
export const fanModeEnum = pgEnum("fan_mode", [
  "auto",
  "manual",
  "manual_off",
  "failsafe",
]);

// ── devices ───────────────────────────────────────────────────────────────

/** Cadastro do ESP32. Uma linha, na prática — mas nada aqui assume isso. */
export const devices = pgTable("devices", {
  deviceId: text("device_id").primaryKey(),
  name: text("name").notNull().default("Aquário"),
  fwVersion: text("fw_version"),
  lastSeenAt: timestamp("last_seen_at", { withTimezone: true }),
  lastIp: text("last_ip"),
  /** Espelha `devices.last_seen_at` contra o limite de silêncio. */
  online: boolean("online").notNull().default(false),
  createdAt: timestamp("created_at", { withTimezone: true })
    .notNull()
    .defaultNow(),
});

// ── device_state ──────────────────────────────────────────────────────────

/**
 * **Uma linha por dispositivo** com o estado corrente — exatamente o papel que
 * as mensagens retidas do MQTT cumpriam. É a razão de o banco existir.
 */
export const deviceState = pgTable("device_state", {
  deviceId: text("device_id")
    .primaryKey()
    .references(() => devices.deviceId, { onDelete: "cascade" }),

  lightOn: boolean("light_on").notNull().default(false),
  lightSource: lightSourceEnum("light_source").notNull().default("boot"),
  /**
   * Último estado que o servidor mandou a luminária assumir, guardado quando o
   * comando é confirmado. A divergência entre isto e `lightOn` é a **única**
   * evidência possível de falha no SSR, que não tem realimentação.
   */
  lightDesired: boolean("light_desired"),

  tempCelsius: real("temp_celsius"),
  tempAvailable: boolean("temp_available").notNull().default(false),
  tempValid: boolean("temp_valid").notNull().default(false),
  /**
   * `bigint` e nulo, não `integer`: a idade vem de `millis()` e cabe em
   * `uint32`, que não cabe num `integer` do PostgreSQL. Nulo é "nunca houve
   * leitura válida" — ver `temperatureStateSchema` no contrato.
   */
  tempAgeMs: bigint("temp_age_ms", { mode: "number" }),

  fanOn: boolean("fan_on").notNull().default(false),
  fanSpeedPercent: smallint("fan_speed_percent").notNull().default(0),
  fanRpm: integer("fan_rpm").notNull().default(0),
  fanMode: fanModeEnum("fan_mode").notNull().default("auto"),

  rtcAvailable: boolean("rtc_available").notNull().default(false),
  rtcTime: text("rtc_time"),
  rtcLostPower: boolean("rtc_lost_power").notNull().default(false),

  wifiRssi: smallint("wifi_rssi"),
  wifiIp: text("wifi_ip"),
  wifiReconnects: integer("wifi_reconnects").notNull().default(0),

  /** `millis()` do dispositivo: estoura `integer` com ~25 dias ligado. */
  uptimeMs: bigint("uptime_ms", { mode: "number" }),
  configVersion: integer("config_version").notNull().default(0),

  /**
   * Guarda de idempotência do ingest.
   *
   * O particionamento impede um `UNIQUE (device_id, boot_id, seq)` de verdade
   * — o PostgreSQL exige que a chave de partição entre em toda constraint
   * única. A dedupe vive aqui: um POST cujo `seq` não avança dentro do mesmo
   * `boot_id` é descartado. Uma linha, acesso por PK, e cobre o reenvio real,
   * que acontece em segundos.
   */
  lastBootId: integer("last_boot_id").notNull().default(-1),
  lastSeq: integer("last_seq").notNull().default(-1),

  /** Carimbado pelo servidor — autoritativo. */
  updatedAt: timestamp("updated_at", { withTimezone: true })
    .notNull()
    .defaultNow(),
  /**
   * Quando a última linha de histórico foi gravada. É o que implementa o
   * heartbeat: sem isso seria preciso um `max(received_at)` sobre a tabela
   * particionada a cada POST.
   */
  lastHistoryAt: timestamp("last_history_at", { withTimezone: true }),
  /** O que o RTC do dispositivo achava que era. Só para diagnóstico de deriva. */
  deviceTime: timestamp("device_time", { withTimezone: true }),
});

// ── component_status ──────────────────────────────────────────────────────

/**
 * Como cada componente **está agora**. Só grava na transição, então é uma
 * tabela minúscula que nunca cresce — e é ela que alimenta a aba Saúde sem
 * varrer histórico.
 */
export const componentStatus = pgTable(
  "component_status",
  {
    deviceId: text("device_id")
      .notNull()
      .references(() => devices.deviceId, { onDelete: "cascade" }),
    comp: componentEnum("comp").notNull(),
    status: healthStatusEnum("status").notNull().default("unknown"),
    /** Desde quando está neste status. É o "sumiu às 14:02". */
    since: timestamp("since", { withTimezone: true }).notNull().defaultNow(),
    lastOkAt: timestamp("last_ok_at", { withTimezone: true }),
    /** O código do evento que causou a transição atual. */
    lastCode: text("last_code"),
    /** Valor corrente relevante: `{"celsius":26.5}`, `{"rpm":1380}`. */
    detail: jsonb("detail").$type<Record<string, unknown>>(),
    updatedAt: timestamp("updated_at", { withTimezone: true })
      .notNull()
      .defaultNow(),
  },
  (t) => [primaryKey({ columns: [t.deviceId, t.comp] })],
);

// ── telemetry (PARTICIONADA POR MÊS) ──────────────────────────────────────

/**
 * Histórico bruto. Gravado **on-change + heartbeat de 60 s**, não a cada POST:
 * o dispositivo fala a cada 3 s, mas só vira linha quando algo muda ou passa
 * um minuto. Dá ~2.000 linhas/dia.
 *
 * `receivedAt` é a chave de partição e por isso encabeça a PK.
 */
export const telemetry = pgTable(
  "telemetry",
  {
    receivedAt: timestamp("received_at", { withTimezone: true })
      .notNull()
      .defaultNow(),
    deviceId: text("device_id").notNull(),
    bootId: integer("boot_id").notNull(),
    seq: integer("seq").notNull(),
    deviceTime: timestamp("device_time", { withTimezone: true }),

    lightOn: boolean("light_on").notNull(),
    tempCelsius: real("temp_celsius"),
    tempValid: boolean("temp_valid").notNull().default(false),
    fanOn: boolean("fan_on").notNull(),
    fanSpeedPercent: smallint("fan_speed_percent").notNull(),
    fanRpm: integer("fan_rpm").notNull(),
    fanMode: fanModeEnum("fan_mode").notNull(),
    wifiRssi: smallint("wifi_rssi"),
  },
  (t) => [
    primaryKey({
      columns: [t.receivedAt, t.deviceId, t.bootId, t.seq],
    }),
    /** BRIN: a tabela cresce em ordem de tempo, então o índice fica minúsculo. */
    index("telemetry_received_at_brin")
      .using("brin", t.receivedAt)
      .with({ pages_per_range: 32 }),
  ],
);

// ── events (PARTICIONADA POR MÊS) ─────────────────────────────────────────

/**
 * Os logs — do firmware e do próprio servidor. Substitui o buffer circular de
 * 30 entradas em RAM que existia no sistema antigo e sumia a cada reboot.
 */
export const events = pgTable(
  "events",
  {
    receivedAt: timestamp("received_at", { withTimezone: true })
      .notNull()
      .defaultNow(),
    id: text("id")
      .notNull()
      .default(sql`gen_random_uuid()`),
    deviceId: text("device_id").notNull(),
    /** `device` ou `server`. Quando o ESP32 está mudo, quem fala é o servidor. */
    source: eventSourceEnum("source").notNull(),
    sev: severityEnum("sev").notNull(),
    comp: componentEnum("comp").notNull(),
    /** `componente.motivo` — estável, legível por máquina. */
    code: text("code").notNull(),
    msg: text("msg").notNull(),
    ctx: jsonb("ctx").$type<Record<string, unknown>>(),
    /** Repetições colapsadas pelo firmware antes de drenar. */
    repeatCount: integer("repeat_count").notNull().default(1),
    /** Relógio do dispositivo; nulo se o RTC sumiu. */
    deviceTime: timestamp("device_time", { withTimezone: true }),
  },
  (t) => [
    primaryKey({ columns: [t.receivedAt, t.id] }),
    /** Sustenta o filtro da aba Logs: por componente e por severidade, recentes primeiro. */
    index("events_filter_idx").on(t.deviceId, t.comp, t.sev, t.receivedAt),
    index("events_code_idx").on(t.deviceId, t.code, t.receivedAt),
  ],
);

// ── telemetry_hourly ──────────────────────────────────────────────────────

/**
 * Rollup horário. **Isento da purga por tamanho** — são poucos megabytes por
 * ano e é o que mantém os relatórios funcionando sobre períodos cuja
 * telemetria bruta já foi descartada.
 */
export const telemetryHourly = pgTable(
  "telemetry_hourly",
  {
    deviceId: text("device_id")
      .notNull()
      .references(() => devices.deviceId, { onDelete: "cascade" }),
    /** Início da hora, em UTC. */
    hour: timestamp("hour", { withTimezone: true }).notNull(),
    tempMin: real("temp_min"),
    tempAvg: real("temp_avg"),
    tempMax: real("temp_max"),
    /** Minutos com a luminária acesa dentro da hora. */
    lightMinutes: smallint("light_minutes").notNull().default(0),
    /** Minutos com a ventoinha girando dentro da hora. */
    fanMinutes: smallint("fan_minutes").notNull().default(0),
    fanRpmAvg: integer("fan_rpm_avg"),
    /** Quantas amostras entraram na agregação — indica lacunas. */
    samples: integer("samples").notNull().default(0),
  },
  (t) => [primaryKey({ columns: [t.deviceId, t.hour] })],
);

// ── commands ──────────────────────────────────────────────────────────────

/**
 * Fila de comandos com a trilha inteira. É daqui que sai a resposta para
 * "por que a luminária não acendeu quando eu apertei o botão".
 */
export const commands = pgTable(
  "commands",
  {
    id: integer("id").generatedAlwaysAsIdentity().primaryKey(),
    deviceId: text("device_id")
      .notNull()
      .references(() => devices.deviceId, { onDelete: "cascade" }),
    /** Dois comandos com o mesmo alvo se anulam — o antigo vira `superseded`. */
    target: commandTargetEnum("target").notNull(),
    action: commandActionEnum("action").notNull(),
    payload: jsonb("payload").$type<Record<string, unknown>>().notNull(),
    status: commandStatusEnum("status").notNull().default("queued"),
    /** E-mail vindo do `Cf-Access-Authenticated-User-Email`. Nulo se automático. */
    requestedBy: text("requested_by"),
    createdAt: timestamp("created_at", { withTimezone: true })
      .notNull()
      .defaultNow(),
    /** Quando entrou numa resposta de POST. */
    sentAt: timestamp("sent_at", { withTimezone: true }),
    /** Quando chegou a um estado terminal, qualquer que seja. */
    settledAt: timestamp("settled_at", { withTimezone: true }),
    /** O motivo, quando não deu certo: `light.gpio_fault`, `cmd.expired`. */
    errorCode: text("error_code"),
  },
  (t) => [
    /** A consulta do caminho quente: comandos a entregar neste POST. */
    index("commands_pending_idx")
      .on(t.deviceId, t.createdAt)
      .where(sql`${t.status} in ('queued', 'sent')`),
    index("commands_history_idx").on(t.deviceId, t.createdAt),
  ],
);

// ── settings ──────────────────────────────────────────────────────────────

/**
 * Configuração do aquário, com o **servidor como fonte de verdade**. O NVS do
 * ESP32 vira cache offline: ele compara `config_version` a cada POST e aplica
 * quando diverge, mas continua operando sozinho se o servidor sumir.
 */
export const settings = pgTable("settings", {
  deviceId: text("device_id")
    .primaryKey()
    .references(() => devices.deviceId, { onDelete: "cascade" }),
  /** Validado por `deviceConfigSchema` antes de gravar. */
  config: jsonb("config").$type<Record<string, unknown>>().notNull(),
  /** Incrementa a cada alteração. É o que o dispositivo compara. */
  configVersion: integer("config_version").notNull().default(1),
  updatedAt: timestamp("updated_at", { withTimezone: true })
    .notNull()
    .defaultNow(),
  updatedBy: text("updated_by"),
});

// ── Componentes exibidos na aba Saúde ─────────────────────────────────────

/** Reexportado para o servidor semear `component_status` no primeiro contato. */
export { AQUARIUM_COMPONENTS };
