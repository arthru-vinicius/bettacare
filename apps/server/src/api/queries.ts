import {
  commandActionSchema,
  deviceConfigSchema,
  summarizeHealth,
  targetOf,
  type CommandAction,
  type ComponentHealth,
  type DeviceConfig,
  type DiagnosticResult,
} from "@bettacare/contract";
import { and, asc, desc, eq, gte, inArray, lt, lte, sql, type SQL } from "drizzle-orm";

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
          // Nulo enquanto o dispositivo nunca falou sobre o alimentador —
          // firmware anterior a esta integração. A partir daí o bloco sempre
          // existe, e é `connected` que diz se é agora ou "última vez".
          feeder:
            state.feederHour1 === null && state.feederAutoEnabled === null
              ? null
              : {
                  connected: state.feederConnected,
                  auto_enabled: state.feederAutoEnabled ?? undefined,
                  hour1: state.feederHour1 ?? undefined,
                  hour2: state.feederHour2 ?? undefined,
                  grains_per_feeding: state.feederGrainsPerFeeding ?? undefined,
                  last_feed_age_s: state.feederLastFeedAt
                    ? Math.max(
                        0,
                        Math.round((Date.now() - state.feederLastFeedAt.getTime()) / 1000),
                      )
                    : null,
                  last_feed_requested: state.feederLastFeedRequested ?? undefined,
                  last_feed_confirmed: state.feederLastFeedConfirmed ?? undefined,
                  last_feed_ok: state.feederLastFeedOk ?? undefined,
                  meals_24h: state.feederMeals24h ?? undefined,
                },
          uptime_ms: state.uptimeMs,
          updated_at: state.updatedAt.toISOString(),
          config_version: state.configVersion,
          controller: {
            reset_reason: state.resetReason,
            free_heap: state.freeHeap,
            min_free_heap: state.minFreeHeap,
            max_alloc_heap: state.maxAllocHeap,
            post_latency_ms: state.postLatencyMs,
            api_failures: state.apiFailures,
            nvs_failures: state.nvsFailures,
            ota_last_result: state.otaLastResult,
            events_dropped: state.eventsDropped,
          },
        }
      : null,
    config: parsedConfig?.success ? parsedConfig.data : null,
    config_version: cfgRow?.configVersion ?? 0,
    diagnostic:
      state?.diagnosticRanAt != null && state.diagnosticOverall != null
        ? {
            ran_at: state.diagnosticRanAt.toISOString(),
            command_id: state.diagnosticCommandId,
            duration_ms: state.diagnosticDurationMs ?? 0,
            overall: state.diagnosticOverall,
            checks: (state.diagnosticChecks ?? []) as DiagnosticResult["checks"],
          }
        : null,
  };
}

/** O que filtra eventos — o mesmo para a lista paginada e para a planilha. */
export interface EventFilterBase {
  deviceId: string;
  comps?: string[] | undefined;
  sevs?: string[] | undefined;
  source?: "device" | "server" | undefined;
  search?: string | undefined;
  /** Início do período, inclusivo. */
  from?: Date | undefined;
  /** Fim do período, exclusivo. */
  to?: Date | undefined;
}

export interface EventFilter extends EventFilterBase {
  limit: number;
  /** `<iso>|<uuid>` do último item da página anterior. */
  cursor?: string | undefined;
}

/**
 * As condições de um filtro de eventos. Compartilhadas com a exportação
 * (`export.ts`): a planilha tem de conter exatamente o que a lista mostra.
 */
export function eventFilterWhere(f: EventFilterBase): SQL[] {
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
    // `%` e `_` digitados são texto, não curinga do ILIKE.
    const termo = `%${f.search.replace(/[\\%_]/g, (m) => `\\${m}`)}%`;
    where.push(sql`(${events.msg} ilike ${termo} or ${events.code} ilike ${termo})`);
  }
  if (f.from) where.push(gte(events.receivedAt, f.from));
  if (f.to) where.push(lt(events.receivedAt, f.to));
  return where;
}

/**
 * Página de logs, do mais recente para o mais antigo.
 *
 * Paginação por **cursor**, não por offset: a tabela recebe linhas o tempo
 * todo, e com offset uma inserção entre duas páginas faria o usuário ver o
 * mesmo evento duas vezes ou pular um.
 */
export async function listEvents(db: Db, f: EventFilter) {
  const where = eventFilterWhere(f);

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

/**
 * Grava a configuração e incrementa a versão que o dispositivo compara.
 *
 * Registra a mudança como evento (UPGRADE/07): antes, mudar o horário da luz
 * não deixava rastro nenhum, e "por que a luz acendeu às 9h?" não tinha
 * resposta na aba de registros. Uma gravação que não muda nada não sobe a
 * versão — senão o dispositivo reaplicaria a mesma configuração à toa.
 */
export async function updateSettings(
  db: Db,
  deviceId: string,
  config: unknown,
  updatedBy: string | null,
) {
  const parsed = deviceConfigSchema.safeParse(config);
  if (!parsed.success) return { ok: false as const, issues: parsed.error.issues };
  const nova = parsed.data;

  return db.transaction(async (tx) => {
    const [atual] = await tx
      .select()
      .from(settings)
      .where(eq(settings.deviceId, deviceId))
      .limit(1);

    const anteriorParse = atual ? deviceConfigSchema.safeParse(atual.config) : null;
    const anterior = anteriorParse?.success ? anteriorParse.data : null;
    const mudancas = anterior ? describeConfigChanges(anterior, nova) : ["configuração inicial gravada"];

    if (atual && mudancas.length === 0) {
      return { ok: true as const, version: atual.configVersion, changed: false };
    }

    const now = new Date();
    const [row] = await tx
      .insert(settings)
      .values({ deviceId, config: nova, configVersion: 1, updatedAt: now, updatedBy })
      .onConflictDoUpdate({
        target: settings.deviceId,
        set: {
          config: nova,
          configVersion: sql`${settings.configVersion} + 1`,
          updatedAt: now,
          updatedBy,
        },
      })
      .returning({ version: settings.configVersion });

    await tx.insert(events).values({
      receivedAt: now,
      deviceId,
      source: "server",
      sev: "info",
      comp: "system",
      code: "settings.updated",
      msg: `${mudancas.join("; ")}${updatedBy ? ` — por ${updatedBy}` : ""}`,
      ctx: { by: updatedBy, before: anterior, after: nova, version: row!.version },
    });

    return { ok: true as const, version: row!.version, changed: true };
  });
}

/** O que mudou, em frases curtas para a aba de registros. */
function describeConfigChanges(a: DeviceConfig, b: DeviceConfig): string[] {
  const graus = (c: number) => `${c.toFixed(1).replace(".", ",")} °C`;
  const segundos = (ms: number) => `${(ms / 1000).toLocaleString("pt-BR")} s`;
  const partes: string[] = [];

  if (a.light_on_time !== b.light_on_time || a.light_off_time !== b.light_off_time) {
    partes.push(
      `Luminária: acende ${b.light_on_time} e apaga ${b.light_off_time} (era ${a.light_on_time}–${a.light_off_time})`,
    );
  }
  if (a.fan_trigger_c !== b.fan_trigger_c || a.fan_off_c !== b.fan_off_c) {
    partes.push(
      `Ventoinha: liga acima de ${graus(b.fan_trigger_c)} e desliga abaixo de ${graus(b.fan_off_c)} (era ${graus(a.fan_trigger_c)}/${graus(a.fan_off_c)})`,
    );
  }
  if (a.telemetry_interval_ms !== b.telemetry_interval_ms) {
    partes.push(
      `Atualização a cada ${segundos(b.telemetry_interval_ms)} (era ${segundos(a.telemetry_interval_ms)})`,
    );
  }
  if (a.heartbeat_interval_ms !== b.heartbeat_interval_ms) {
    partes.push(
      `Histórico mínimo a cada ${segundos(b.heartbeat_interval_ms)} (era ${segundos(a.heartbeat_interval_ms)})`,
    );
  }
  return partes;
}
