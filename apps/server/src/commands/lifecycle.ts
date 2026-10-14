import { NON_REPEATABLE_ACTIONS } from "@bettacare/contract";
import { and, eq, inArray, isNull, lt, or } from "drizzle-orm";

import type { Db } from "../db/client.js";
import { commands, events } from "../db/schema.js";

/**
 * O prazo de vida de um comando, num lugar só.
 *
 * Três partes do servidor precisam da mesma resposta para "este comando ainda
 * vale?": o watchdog (que encerra os vencidos), o ingest (que não pode
 * entregar um vencido) e a lista que a interface mostra (que diz "até as
 * 14:17"). Com a regra copiada nos três, bastava um divergir para a tela
 * prometer uma entrega que o ingest já tinha descartado.
 */

type Tx = Parameters<Parameters<Db["transaction"]>[0]>[0];
type Exec = Db | Tx;

/**
 * Folga para a confirmação de um comando entregue perto do fim do prazo.
 *
 * Sem ela, um comando de 14 min entregue no último minuto vencia antes de o
 * `ack` ter tempo de voltar — o aquário executava, e a tela dizia "expirou".
 * Dois minutos são dezenas de POSTs: se a confirmação não veio nisso, a
 * conexão caiu de novo, e aí o prazo normal já tinha dado a chance dele.
 */
export const ACK_GRACE_MS = 2 * 60 * 1000;

export interface CommandTiming {
  status: string;
  createdAt: Date;
  sentAt: Date | null;
}

/** Até quando o comando ainda vale. Nulo depois de encerrado. */
export function commandExpiresAt(c: CommandTiming, maxAgeS: number): Date | null {
  if (c.status !== "queued" && c.status !== "sent") return null;
  const prazo = c.createdAt.getTime() + maxAgeS * 1000;
  if (c.status === "sent" && c.sentAt !== null) {
    return new Date(Math.max(prazo, c.sentAt.getTime() + ACK_GRACE_MS));
  }
  return new Date(prazo);
}

export function isCommandOverdue(c: CommandTiming, maxAgeS: number, now: Date): boolean {
  const ate = commandExpiresAt(c, maxAgeS);
  return ate !== null && now.getTime() >= ate.getTime();
}

function minutos(s: number): string {
  const min = Math.round(s / 60);
  return min === 1 ? "1 min" : `${min} min`;
}

/**
 * Encerra os comandos vencidos — de um dispositivo, ou de todos.
 *
 * Dois desfechos, porque são duas histórias diferentes para quem lê:
 *
 * - **Nunca entregue** (`queued`): o aquário passou o prazo inteiro sem
 *   contato. Nada foi executado — `cmd.stale_discarded`.
 * - **Entregue sem confirmação** (`sent`): foi numa resposta de POST, talvez
 *   mais de uma, e o `ack` nunca voltou. Pode ter sido executado —
 *   `cmd.expired`. Se a confirmação aparecer depois, o ingest registra
 *   `cmd.late_ack`.
 */
export async function expireOverdueCommands(
  tx: Exec,
  now: Date,
  maxAgeS: number,
  deviceId?: string,
): Promise<number> {
  const prazo = new Date(now.getTime() - maxAgeS * 1000);
  const folga = new Date(now.getTime() - ACK_GRACE_MS);
  const doDispositivo = deviceId === undefined ? undefined : eq(commands.deviceId, deviceId);
  const campos = { id: commands.id, deviceId: commands.deviceId, action: commands.action };

  const descartados = await tx
    .update(commands)
    .set({ status: "expired", settledAt: now, errorCode: "cmd.stale_discarded" })
    .where(and(eq(commands.status, "queued"), lt(commands.createdAt, prazo), doDispositivo))
    .returning(campos);

  const expirados = await tx
    .update(commands)
    .set({ status: "expired", settledAt: now, errorCode: "cmd.expired" })
    .where(
      and(
        eq(commands.status, "sent"),
        lt(commands.createdAt, prazo),
        or(isNull(commands.sentAt), lt(commands.sentAt, folga)),
        doDispositivo,
      ),
    )
    .returning(campos);

  for (const cmd of descartados) {
    await tx.insert(events).values({
      receivedAt: now,
      deviceId: cmd.deviceId,
      source: "server",
      sev: "warn",
      comp: "api",
      code: "cmd.stale_discarded",
      msg: `${cmd.action} descartado sem executar — o aquário ficou mais de ${minutos(maxAgeS)} sem contato`,
      ctx: { command_id: cmd.id, action: cmd.action },
      repeatCount: 1,
      deviceTime: null,
    });
  }

  for (const cmd of expirados) {
    await tx.insert(events).values({
      receivedAt: now,
      deviceId: cmd.deviceId,
      source: "server",
      sev: "error",
      comp: "api",
      code: "cmd.expired",
      msg: `${cmd.action} reenviado por ${minutos(maxAgeS)} sem confirmação — a conexão com o aquário caiu antes`,
      ctx: { command_id: cmd.id, action: cmd.action },
      repeatCount: 1,
      deviceTime: null,
    });
  }

  return descartados.length + expirados.length;
}

/**
 * O dispositivo reiniciou (`boot_id` novo) com comandos já entregues e sem
 * confirmação. Os repetíveis seguem na fila e são reentregues normalmente;
 * os que não podem ser repetidos (`NON_REPEATABLE_ACTIONS`) são encerrados
 * aqui, porque o firmware perdeu no reinício a memória do que já executou:
 *
 * - `device.reboot`: o reinício pedido **aconteceu** — confirmado.
 * - `feeder.feed_now`: pode ter saído ou não, e repetir seria dobrar a
 *   ração. Encerrado como `cmd.interrupted`, com um evento que diz o que
 *   conferir.
 */
export async function settleAfterReboot(tx: Exec, deviceId: string, now: Date): Promise<void> {
  const pendentes = await tx
    .select({ id: commands.id, action: commands.action })
    .from(commands)
    .where(
      and(
        eq(commands.deviceId, deviceId),
        eq(commands.status, "sent"),
        inArray(commands.action, [...NON_REPEATABLE_ACTIONS]),
      ),
    );

  for (const cmd of pendentes) {
    if (cmd.action === "device.reboot") {
      await tx
        .update(commands)
        .set({ status: "acked", settledAt: now, errorCode: null })
        .where(eq(commands.id, cmd.id));
      continue;
    }

    await tx
      .update(commands)
      .set({ status: "expired", settledAt: now, errorCode: "cmd.interrupted" })
      .where(eq(commands.id, cmd.id));

    await tx.insert(events).values({
      receivedAt: now,
      deviceId,
      source: "server",
      sev: "warn",
      comp: "api",
      code: "cmd.interrupted",
      msg: `O controlador reiniciou antes de confirmar ${cmd.action}; o pedido não será repetido sozinho`,
      ctx: { command_id: cmd.id, action: cmd.action },
      repeatCount: 1,
      deviceTime: null,
    });
  }
}
