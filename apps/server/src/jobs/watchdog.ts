import { and, eq, lt, sql } from "drizzle-orm";

import { commands, devices, events } from "../db/schema.js";
import { sendToAll } from "../push/notify.js";
import type { Runtime } from "../runtime.js";

/** Um `UPDATE` numa linha a cada 30 s — desprezível num servidor de 2 núcleos. */
const INTERVAL_MS = 30_000;

/**
 * Silêncio a partir do qual um comando ainda `queued` (nunca entregue) já
 * merece `cmd.device_offline` — bem mais curto que `DEVICE_OFFLINE_AFTER_S`
 * (padrão 60 s), porque o usuário que acabou de tocar um botão no app quer
 * saber em segundos, não em um minuto, por que nada aconteceu ainda.
 * Ver UPGRADE/04 (S4) e a seção 4 de `docs/arquitetura-observabilidade.md`.
 */
const QUEUED_OFFLINE_AFTER_S = 15;

/**
 * O que só o servidor consegue perceber: silêncio.
 *
 * O dispositivo nunca vai avisar que caiu — se pudesse avisar, não teria
 * caído. Todo diagnóstico de ausência mora aqui.
 */
export function startWatchdog(rt: Runtime): () => void {
  const timer = setInterval(() => {
    void tick(rt).catch((err) =>
      rt.log.error({ err }, "falha no watchdog"),
    );
  }, INTERVAL_MS);

  // Não segura o processo aberto no encerramento.
  timer.unref();
  return () => clearInterval(timer);
}

export async function tick(rt: Runtime): Promise<void> {
  if (!rt.isDbReady()) return;

  const now = new Date();
  /** Coletado dentro da transação, notificado depois que ela fecha. */
  const offline: string[] = [];
  const offlineAt = new Date(now.getTime() - rt.cfg.DEVICE_OFFLINE_AFTER_S * 1000);
  const expiredAt = new Date(now.getTime() - rt.cfg.COMMAND_TTL_S * 1000);

  await rt.db.transaction(async (tx) => {
    // ── Dispositivos que ficaram mudos ────────────────────────────────────
    const caidos = await tx
      .update(devices)
      .set({ online: false })
      .where(and(eq(devices.online, true), lt(devices.lastSeenAt, offlineAt)))
      .returning({ deviceId: devices.deviceId, lastSeenAt: devices.lastSeenAt });

    for (const d of caidos) {
      await tx.insert(events).values({
        receivedAt: now,
        deviceId: d.deviceId,
        source: "server",
        sev: "error",
        comp: "system",
        code: "device.offline",
        msg: `Sem contato há mais de ${rt.cfg.DEVICE_OFFLINE_AFTER_S}s`,
        ctx: { last_seen_at: d.lastSeenAt?.toISOString() ?? null },
      });

      // Marca todos os componentes como sem informação: manter "ok" com dado
      // velho seria afirmar algo que não se sabe mais.
      await tx.execute(sql`
        update component_status
           set status = 'unknown', since = ${now}, updated_at = ${now}
         where device_id = ${d.deviceId} and status <> 'unknown'
      `);

      rt.log.warn({ device: d.deviceId }, "dispositivo offline");
      offline.push(d.deviceId);
    }

    // ── Comandos entregues que nunca foram confirmados ────────────────────
    const expirados = await tx
      .update(commands)
      .set({ status: "expired", settledAt: now, errorCode: "cmd.expired" })
      .where(and(eq(commands.status, "sent"), lt(commands.sentAt, expiredAt)))
      .returning({ id: commands.id, deviceId: commands.deviceId, action: commands.action });

    for (const cmd of expirados) {
      await tx.insert(events).values({
        receivedAt: now,
        deviceId: cmd.deviceId,
        source: "server",
        sev: "error",
        comp: "api",
        code: "cmd.expired",
        msg: `O comando ${cmd.action} foi entregue mas nunca confirmado`,
        ctx: { command_id: cmd.id, action: cmd.action },
      });

      rt.log.warn({ command: cmd.id, action: cmd.action }, "comando expirou");
    }

    // ── Comandos que o dispositivo nunca chegou a receber ──────────────────
    // Causa diferente de `expired`: aqui o comando não foi entregue nenhuma
    // vez, porque não há POST há mais de `QUEUED_OFFLINE_AFTER_S`. Não muda o
    // status — o comando continua legitimamente esperando o dispositivo
    // voltar —, só registra o evento uma vez (`errorCode` como marca de "já
    // avisado", limpo quando o comando finalmente é entregue).
    const queuedOfflineAt = new Date(now.getTime() - QUEUED_OFFLINE_AFTER_S * 1000);
    const semContato = await tx
      .select({
        id: commands.id,
        deviceId: commands.deviceId,
        action: commands.action,
        errorCode: commands.errorCode,
      })
      .from(commands)
      .innerJoin(devices, eq(devices.deviceId, commands.deviceId))
      .where(
        and(eq(commands.status, "queued"), lt(devices.lastSeenAt, queuedOfflineAt)),
      );

    for (const cmd of semContato) {
      if (cmd.errorCode === "cmd.device_offline") continue;

      await tx
        .update(commands)
        .set({ errorCode: "cmd.device_offline" })
        .where(eq(commands.id, cmd.id));

      await tx.insert(events).values({
        receivedAt: now,
        deviceId: cmd.deviceId,
        source: "server",
        sev: "error",
        comp: "api",
        code: "cmd.device_offline",
        msg: `${cmd.action} aguardando na fila — dispositivo sem contato`,
        ctx: { command_id: cmd.id, action: cmd.action },
      });

      rt.log.warn(
        { command: cmd.id, action: cmd.action },
        "comando aguardando dispositivo offline",
      );
    }
  });

  // Fora da transação: uma entrega a push service externo pode demorar, e não
  // deve segurar conexão do pool nem atrasar o próximo tick do watchdog.
  for (const _ of offline) {
    await sendToAll(rt, {
      title: "Controlador sem contato",
      body: `O aquário parou de responder há mais de ${rt.cfg.DEVICE_OFFLINE_AFTER_S}s. Verifique a energia e a rede.`,
      tag: "device-offline",
      requireInteraction: true,
    }).catch((err) => rt.log.error({ err }, "falha ao notificar dispositivo offline"));
  }
}
