import {
  COMPONENT_LABELS,
  DEFAULT_CONFIG,
  deviceConfigSchema,
  HEALTH_STATUS_LABELS,
  type Component,
  type DeviceConfig,
  type PendingCommand,
  type TelemetryRequest,
  type TelemetryResponse,
} from "@bettacare/contract";
import { and, asc, eq, inArray } from "drizzle-orm";

import type { AppConfig } from "../config.js";
import type { Db } from "../db/client.js";
import {
  commands,
  componentStatus,
  deviceState,
  devices,
  events,
  settings,
  telemetry,
} from "../db/schema.js";
import { evaluateHealth, shouldWriteHistory } from "../health/evaluate.js";

/**
 * Quantos comandos cabem numa resposta. O corpo precisa continuar pequeno e
 * previsível — o ESP32 tem memória escassa — e na prática nunca há mais que um
 * ou dois pendentes, porque comandos do mesmo alvo se anulam.
 */
const MAX_COMMANDS_PER_RESPONSE = 4;

export interface ProcessResult {
  response: TelemetryResponse;
  /** Verdadeiro quando o POST foi um reenvio e nada foi gravado. */
  duplicate: boolean;
  /** Se uma linha de histórico foi escrita neste ciclo. */
  wroteHistory: boolean;
}

export async function processTelemetry(
  db: Db,
  cfg: AppConfig,
  body: TelemetryRequest,
  now: Date,
): Promise<ProcessResult> {
  return db.transaction(async (tx) => {
    // ── 1. O dispositivo existe e acabou de falar ────────────────────────
    const [before] = await tx
      .select({ online: devices.online })
      .from(devices)
      .where(eq(devices.deviceId, body.device_id))
      .limit(1);

    await tx
      .insert(devices)
      .values({
        deviceId: body.device_id,
        lastSeenAt: now,
        lastIp: body.wifi.ip,
        fwVersion: body.fw_version ?? null,
        online: true,
      })
      .onConflictDoUpdate({
        target: devices.deviceId,
        set: {
          lastSeenAt: now,
          lastIp: body.wifi.ip,
          fwVersion: body.fw_version ?? null,
          online: true,
        },
      });

    // O watchdog marca offline; quem percebe a volta é o próprio POST.
    if (before !== undefined && !before.online) {
      await insertServerEvent(tx, body.device_id, now, {
        sev: "info",
        comp: "system",
        code: "device.online",
        msg: "Dispositivo voltou a se comunicar",
      });
    }

    const [previous] = await tx
      .select()
      .from(deviceState)
      .where(eq(deviceState.deviceId, body.device_id))
      .limit(1);

    // ── 2. Confirmações do POST anterior ─────────────────────────────────
    // Processadas mesmo num reenvio: marcar um comando como confirmado é
    // idempotente, e perder um ack custaria uma reentrega desnecessária.
    const lightDesiredFromAck = await applyAcks(tx, body, now);

    // ── 3. Idempotência ──────────────────────────────────────────────────
    // A guarda é o `seq`, não uma constraint única: o particionamento de
    // `telemetry` impede um UNIQUE (device_id, boot_id, seq) de verdade.
    // Um `boot_id` novo zera a contagem — o dispositivo reiniciou.
    const duplicate =
      previous !== undefined &&
      body.boot_id === previous.lastBootId &&
      body.seq <= previous.lastSeq;

    const { config, configVersion } = await loadConfig(tx, body.device_id, now);

    let wroteHistory = false;

    if (!duplicate) {
      const lightDesired =
        lightDesiredFromAck ?? previous?.lightDesired ?? null;

      await recordHealth(tx, body, now, lightDesired);
      await recordDeviceEvents(tx, body, now);

      wroteHistory = shouldWriteHistory(
        body,
        previous
          ? {
              lightOn: previous.lightOn,
              fanOn: previous.fanOn,
              fanSpeedPercent: previous.fanSpeedPercent,
              fanMode: previous.fanMode,
              tempCelsius: previous.tempCelsius,
              tempAvailable: previous.tempAvailable,
              lastHistoryAt: previous.lastHistoryAt,
            }
          : null,
        now,
        config.heartbeat_interval_ms,
      );

      if (wroteHistory) {
        await tx.insert(telemetry).values({
          receivedAt: now,
          deviceId: body.device_id,
          bootId: body.boot_id,
          seq: body.seq,
          deviceTime: body.device_time ? new Date(body.device_time) : null,
          lightOn: body.light.on,
          tempCelsius: body.temperature.celsius,
          tempValid: body.temperature.valid,
          fanOn: body.fan.on,
          fanSpeedPercent: body.fan.speed_percent,
          fanRpm: body.fan.rpm,
          fanMode: body.fan.mode,
          wifiRssi: body.wifi.rssi,
        });
      }

      await upsertState(tx, body, now, {
        lightDesired,
        lastHistoryAt: wroteHistory ? now : (previous?.lastHistoryAt ?? null),
      });
    }

    // ── 4. Comandos para a resposta ──────────────────────────────────────
    const pending = await takePendingCommands(tx, body.device_id, now);

    const response: TelemetryResponse = {
      ok: true,
      server_time: now.toISOString(),
      config_version: configVersion,
      commands: pending,
      // O bloco só viaja quando o dispositivo está atrasado. No caso normal a
      // resposta fica em torno de 100 bytes.
      ...(body.config_version === configVersion ? {} : { config }),
    };

    return { response, duplicate, wroteHistory };
  });
}

type Tx = Parameters<Parameters<Db["transaction"]>[0]>[0];

/**
 * Aplica os `ack` do dispositivo.
 *
 * Devolve o estado desejado da luminária quando um `light.set` foi confirmado
 * com sucesso — é ele que passa a servir de referência para detectar SSR que
 * não obedece.
 */
async function applyAcks(
  tx: Tx,
  body: TelemetryRequest,
  now: Date,
): Promise<boolean | null> {
  if (body.ack.length === 0) return null;

  const ids = body.ack.map((a) => a.id);
  const rows = await tx
    .select()
    .from(commands)
    .where(
      and(eq(commands.deviceId, body.device_id), inArray(commands.id, ids)),
    );

  let lightDesired: boolean | null = null;

  for (const ack of body.ack) {
    const cmd = rows.find((r) => r.id === ack.id);
    // Um ack para comando já resolvido é reenvio: ignorar em silêncio.
    if (!cmd || (cmd.status !== "sent" && cmd.status !== "queued")) continue;

    await tx
      .update(commands)
      .set({
        status: ack.ok ? "acked" : "rejected",
        settledAt: now,
        errorCode: ack.ok ? null : (ack.code ?? "cmd.rejected"),
      })
      .where(eq(commands.id, ack.id));

    if (!ack.ok) {
      await insertServerEvent(tx, body.device_id, now, {
        sev: "error",
        comp: "api",
        code: "cmd.rejected",
        msg: `O dispositivo recusou ${cmd.action}: ${ack.code ?? "sem motivo informado"}`,
        ctx: { command_id: ack.id, action: cmd.action },
      });
      continue;
    }

    if (cmd.action === "light.set") {
      const on = cmd.payload["on"];
      if (typeof on === "boolean") lightDesired = on;
    }
  }

  return lightDesired;
}

/**
 * Avalia a saúde de cada componente e grava **apenas as transições**.
 *
 * Escrever só na mudança é o que mantém `component_status` minúscula, e é
 * também o que dá sentido ao campo `since`: ele responde "desde quando",
 * não "desde o último POST".
 */
async function recordHealth(
  tx: Tx,
  body: TelemetryRequest,
  now: Date,
  lightDesired: boolean | null,
): Promise<void> {
  const current = await tx
    .select()
    .from(componentStatus)
    .where(eq(componentStatus.deviceId, body.device_id));

  const previousDetail: Partial<
    Record<Component, Record<string, unknown> | null>
  > = {};
  for (const row of current) previousDetail[row.comp] = row.detail;

  const verdicts = evaluateHealth(body, { previousDetail, lightDesired });

  for (const v of verdicts) {
    const before = current.find((r) => r.comp === v.comp);
    const changed = before === undefined || before.status !== v.status;

    await tx
      .insert(componentStatus)
      .values({
        deviceId: body.device_id,
        comp: v.comp,
        status: v.status,
        since: now,
        lastOkAt: v.status === "ok" ? now : null,
        lastCode: v.code,
        detail: v.detail,
        updatedAt: now,
      })
      .onConflictDoUpdate({
        target: [componentStatus.deviceId, componentStatus.comp],
        set: {
          status: v.status,
          // `since` só se move na transição — é o "desde quando".
          ...(changed ? { since: now } : {}),
          ...(v.status === "ok" ? { lastOkAt: now } : {}),
          lastCode: v.code,
          detail: v.detail,
          updatedAt: now,
        },
      });

    // Transição registrada como evento do servidor: é a trilha auditável de
    // saúde, independente do que o firmware resolveu contar por conta própria.
    if (changed && before !== undefined) {
      const recovered = v.status === "ok";
      const nome = COMPONENT_LABELS[v.comp];
      await insertServerEvent(tx, body.device_id, now, {
        sev: recovered ? "info" : v.status === "degraded" ? "warn" : "error",
        comp: v.comp,
        code: v.code ?? `${v.comp}.recovered`,
        // Texto para humano: esta linha aparece na aba Logs do app, e
        // "wifi: unknown → degraded" é jargão interno vazando para o usuário.
        msg: recovered
          ? `${nome} voltou ao normal`
          : `${nome} passou de ${HEALTH_STATUS_LABELS[before.status].toLowerCase()} para ${HEALTH_STATUS_LABELS[v.status].toLowerCase()}`,
        ctx: { from: before.status, to: v.status },
      });
    }
  }
}

async function recordDeviceEvents(
  tx: Tx,
  body: TelemetryRequest,
  now: Date,
): Promise<void> {
  if (body.events.length === 0) return;

  await tx.insert(events).values(
    body.events.map((e) => ({
      receivedAt: now,
      deviceId: body.device_id,
      source: "device" as const,
      sev: e.sev,
      comp: e.comp,
      code: e.code,
      msg: e.msg,
      ctx: e.ctx ?? null,
      repeatCount: e.repeat_count,
      deviceTime: e.t ? new Date(e.t) : null,
    })),
  );
}

async function insertServerEvent(
  tx: Tx,
  deviceId: string,
  now: Date,
  e: {
    sev: "debug" | "info" | "warn" | "error" | "fatal";
    comp: Component;
    code: string;
    msg: string;
    ctx?: Record<string, unknown>;
  },
): Promise<void> {
  await tx.insert(events).values({
    receivedAt: now,
    deviceId,
    source: "server",
    sev: e.sev,
    comp: e.comp,
    code: e.code,
    msg: e.msg,
    ctx: e.ctx ?? null,
    repeatCount: 1,
    deviceTime: null,
  });
}

async function upsertState(
  tx: Tx,
  body: TelemetryRequest,
  now: Date,
  extra: { lightDesired: boolean | null; lastHistoryAt: Date | null },
): Promise<void> {
  const row = {
    deviceId: body.device_id,
    lightOn: body.light.on,
    lightSource: body.light.source,
    lightDesired: extra.lightDesired,
    tempCelsius: body.temperature.celsius,
    tempAvailable: body.temperature.available,
    tempValid: body.temperature.valid,
    tempAgeMs: body.temperature.age_ms,
    fanOn: body.fan.on,
    fanSpeedPercent: body.fan.speed_percent,
    fanRpm: body.fan.rpm,
    fanMode: body.fan.mode,
    rtcAvailable: body.rtc.available,
    rtcTime: body.rtc.time,
    rtcLostPower: body.rtc.lost_power,
    wifiRssi: body.wifi.rssi,
    wifiIp: body.wifi.ip,
    wifiReconnects: body.wifi.reconnects,
    uptimeMs: body.uptime_ms,
    configVersion: body.config_version,
    lastBootId: body.boot_id,
    lastSeq: body.seq,
    updatedAt: now,
    lastHistoryAt: extra.lastHistoryAt,
    deviceTime: body.device_time ? new Date(body.device_time) : null,
  };

  const { deviceId: _ignored, ...updatable } = row;

  await tx
    .insert(deviceState)
    .values(row)
    .onConflictDoUpdate({ target: deviceState.deviceId, set: updatable });
}

/** Lê a configuração do dispositivo, criando o padrão no primeiro contato. */
async function loadConfig(
  tx: Tx,
  deviceId: string,
  now: Date,
): Promise<{ config: DeviceConfig; configVersion: number }> {
  const [row] = await tx
    .select()
    .from(settings)
    .where(eq(settings.deviceId, deviceId))
    .limit(1);

  if (row === undefined) {
    await tx
      .insert(settings)
      .values({
        deviceId,
        config: DEFAULT_CONFIG,
        configVersion: 1,
        updatedAt: now,
      })
      .onConflictDoNothing();
    return { config: DEFAULT_CONFIG, configVersion: 1 };
  }

  const parsed = deviceConfigSchema.safeParse(row.config);
  // Configuração corrompida no banco não pode derrubar o ingest nem deixar o
  // aquário sem regra: cai no padrão e segue.
  return {
    config: parsed.success ? parsed.data : DEFAULT_CONFIG,
    configVersion: row.configVersion,
  };
}

/**
 * Devolve os comandos pendentes e marca os novos como entregues.
 *
 * Um comando **não sai da fila ao ser entregue** — só quando o dispositivo o
 * confirma no `ack`. Se a resposta se perder, ele é reentregue no POST
 * seguinte, que é exatamente o que o contrato exige.
 */
async function takePendingCommands(
  tx: Tx,
  deviceId: string,
  now: Date,
): Promise<PendingCommand[]> {
  const rows = await tx
    .select()
    .from(commands)
    .where(
      and(
        eq(commands.deviceId, deviceId),
        inArray(commands.status, ["queued", "sent"]),
      ),
    )
    .orderBy(asc(commands.createdAt))
    .limit(MAX_COMMANDS_PER_RESPONSE);

  const novos = rows.filter((r) => r.status === "queued").map((r) => r.id);
  if (novos.length > 0) {
    await tx
      .update(commands)
      .set({ status: "sent", sentAt: now })
      .where(inArray(commands.id, novos));
  }

  return rows.map(
    (r) => ({ id: r.id, action: r.action, ...r.payload }) as PendingCommand,
  );
}
