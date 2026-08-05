import {
  commandActionSchema,
  deviceConfigSchema,
  summarizeHealth,
  targetOf,
  type CommandAction,
  type ComponentHealth,
} from "@bettacare/contract";
import { and, asc, desc, eq, gte, inArray, lte, sql, type SQL } from "drizzle-orm";

import type { Db } from "../db/client.js";
import {
  commands,
  componentStatus,
  deviceState,
  devices,
  events,
  settings,
  telemetryHourly,
} from "../db/schema.js";

/** Estado completo do aquário — o que a tela inicial e a aba Saúde consomem. */
export async function getOverview(db: Db, deviceId: string) {
  const [device] = await db
    .select()
    .from(devices)
    .where(eq(devices.deviceId, deviceId))
    .limit(1);

  if (device === undefined) return null;

  const [state] = await db
    .select()
    .from(deviceState)
    .where(eq(deviceState.deviceId, deviceId))
    .limit(1);

  const health = await db
    .select()
    .from(componentStatus)
    .where(eq(componentStatus.deviceId, deviceId));

  const [cfgRow] = await db
    .select()
    .from(settings)
    .where(eq(settings.deviceId, deviceId))
    .limit(1);

  const components: ComponentHealth[] = health.map((h) => ({
    comp: h.comp,
    status: h.status,
    since: h.since.toISOString(),
    last_ok_at: h.lastOkAt?.toISOString() ?? null,
    last_code: h.lastCode,
    detail: h.detail ?? null,
  }));

  const parsedConfig = cfgRow
    ? deviceConfigSchema.safeParse(cfgRow.config)
    : null;

  return {
    device: {
      device_id: device.deviceId,
      name: device.name,
      fw_version: device.fwVersion,
      online: device.online,
      last_seen_at: device.lastSeenAt?.toISOString() ?? null,
      last_ip: device.lastIp,
    },
    overall: summarizeHealth(components, device.online),
    components,
    state: state
      ? {
          light: {
            on: state.lightOn,
            source: state.lightSource,
            desired: state.lightDesired,
          },
          temperature: {
            celsius: state.tempCelsius,
            available: state.tempAvailable,
            valid: state.tempValid,
            age_ms: state.tempAgeMs,
          },
          fan: {
            on: state.fanOn,
            speed_percent: state.fanSpeedPercent,
            rpm: state.fanRpm,
            mode: state.fanMode,
          },
          rtc: {
            available: state.rtcAvailable,
            time: state.rtcTime,
            lost_power: state.rtcLostPower,
          },
          wifi: {
            rssi: state.wifiRssi,
            ip: state.wifiIp,
            reconnects: state.wifiReconnects,
          },
          uptime_ms: state.uptimeMs,
          updated_at: state.updatedAt.toISOString(),
        }
      : null,
    config: parsedConfig?.success ? parsedConfig.data : null,
    config_version: cfgRow?.configVersion ?? 0,
  };
}

export interface EventFilter {
  deviceId: string;
  comps?: string[] | undefined;
  sevs?: string[] | undefined;
  source?: "device" | "server" | undefined;
  search?: string | undefined;
  limit: number;
  /** `<iso>|<uuid>` do último item da página anterior. */
  cursor?: string | undefined;
}

/**
 * Página de logs, do mais recente para o mais antigo.
 *
 * Paginação por **cursor**, não por offset: a tabela recebe linhas o tempo
 * todo, e com offset uma inserção entre duas páginas faria o usuário ver o
 * mesmo evento duas vezes ou pular um.
 */
export async function listEvents(db: Db, f: EventFilter) {
  const where: SQL[] = [eq(events.deviceId, f.deviceId)];

  if (f.comps?.length) {
    where.push(inArray(events.comp, f.comps as never[]));
  }
  if (f.sevs?.length) {
    where.push(inArray(events.sev, f.sevs as never[]));
  }
  if (f.source) {
    where.push(eq(events.source, f.source));
  }
  if (f.search) {
    where.push(sql`(${events.msg} ilike ${"%" + f.search + "%"} or ${events.code} ilike ${"%" + f.search + "%"})`);
  }
  if (f.cursor) {
    const [ts, id] = f.cursor.split("|");
    if (ts && id) {
      // Comparação de tupla: casa exatamente com a PK (received_at, id) e
      // evita empates quando dois eventos chegam no mesmo milissegundo.
      where.push(
        sql`(${events.receivedAt}, ${events.id}) < (${new Date(ts)}::timestamptz, ${id})`,
      );
    }
  }

  const rows = await db
    .select()
    .from(events)
    .where(and(...where))
    .orderBy(desc(events.receivedAt), desc(events.id))
    .limit(f.limit + 1);

  const hasMore = rows.length > f.limit;
  const page = hasMore ? rows.slice(0, f.limit) : rows;
  const last = page.at(-1);

  return {
    items: page.map((e) => ({
      id: e.id,
      received_at: e.receivedAt.toISOString(),
      device_time: e.deviceTime?.toISOString() ?? null,
      source: e.source,
      sev: e.sev,
      comp: e.comp,
      code: e.code,
      msg: e.msg,
      ctx: e.ctx,
      repeat_count: e.repeatCount,
    })),
    next_cursor:
      hasMore && last ? `${last.receivedAt.toISOString()}|${last.id}` : null,
  };
}

/** Histórico de comandos, com a trilha inteira de cada um. */
export async function listCommands(db: Db, deviceId: string, limit: number) {
  const rows = await db
    .select()
    .from(commands)
    .where(eq(commands.deviceId, deviceId))
    .orderBy(desc(commands.createdAt))
    .limit(limit);

  return rows.map((c) => ({
    id: c.id,
    target: c.target,
    action: c.action,
    payload: c.payload,
    status: c.status,
    requested_by: c.requestedBy,
    created_at: c.createdAt.toISOString(),
    sent_at: c.sentAt?.toISOString() ?? null,
    settled_at: c.settledAt?.toISOString() ?? null,
    error_code: c.errorCode,
  }));
}

/**
 * Enfileira um comando.
 *
 * Comandos do mesmo alvo se anulam: enfileirar um novo marca os anteriores
 * como `superseded`. Sem isso, um dispositivo voltando de meia hora offline
 * executaria em sequência uma fila inteira de decisões já obsoletas — acender,
 * apagar, acender — em vez de simplesmente assumir o estado desejado atual.
 */
export async function enqueueCommand(
  db: Db,
  deviceId: string,
  action: CommandAction,
  requestedBy: string | null,
): Promise<number> {
  const target = targetOf(action);
  const { action: name, ...payload } = action;

  return db.transaction(async (tx) => {
    await tx
      .update(commands)
      .set({ status: "superseded", settledAt: new Date() })
      .where(
        and(
          eq(commands.deviceId, deviceId),
          eq(commands.target, target),
          inArray(commands.status, ["queued", "sent"]),
        ),
      );

    const [row] = await tx
      .insert(commands)
      .values({
        deviceId,
        target,
        action: name,
        payload,
        status: "queued",
        requestedBy,
      })
      .returning({ id: commands.id });

    return row!.id;
  });
}

export function parseCommand(raw: unknown) {
  return commandActionSchema.safeParse(raw);
}

/** Série do relatório, direto do rollup horário. */
export async function getReport(
  db: Db,
  deviceId: string,
  from: Date,
  to: Date,
) {
  const rows = await db
    .select()
    .from(telemetryHourly)
    .where(
      and(
        eq(telemetryHourly.deviceId, deviceId),
        gte(telemetryHourly.hour, from),
        lte(telemetryHourly.hour, to),
      ),
    )
    .orderBy(asc(telemetryHourly.hour));

  return rows.map((r) => ({
    hour: r.hour.toISOString(),
    temp_min: r.tempMin,
    temp_avg: r.tempAvg,
    temp_max: r.tempMax,
    light_minutes: r.lightMinutes,
    fan_minutes: r.fanMinutes,
    fan_rpm_avg: r.fanRpmAvg,
    samples: r.samples,
  }));
}

/** Grava a configuração e incrementa a versão que o dispositivo compara. */
export async function updateSettings(
  db: Db,
  deviceId: string,
  config: unknown,
  updatedBy: string | null,
) {
  const parsed = deviceConfigSchema.safeParse(config);
  if (!parsed.success) return { ok: false as const, issues: parsed.error.issues };

  const [row] = await db
    .insert(settings)
    .values({
      deviceId,
      config: parsed.data,
      configVersion: 1,
      updatedAt: new Date(),
      updatedBy,
    })
    .onConflictDoUpdate({
      target: settings.deviceId,
      set: {
        config: parsed.data,
        configVersion: sql`${settings.configVersion} + 1`,
        updatedAt: new Date(),
        updatedBy,
      },
    })
    .returning({ version: settings.configVersion });

  return { ok: true as const, version: row!.version };
}
