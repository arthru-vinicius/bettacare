import { eq, isNull, or, sql } from "drizzle-orm";
import webpush from "web-push";

import type { Db } from "../db/client.js";
import { pushSubscriptions, serverKeys } from "../db/schema.js";
import type { Runtime } from "../runtime.js";

/**
 * Notificações Web Push.
 *
 * O que isto resolve: um alerta que só aparece com o app aberto não é alerta.
 * O valor de avisar que a luminária parou de responder está justamente em
 * chegar quando **ninguém está olhando** — e é isso que o Web Push entrega, no
 * Android e no Windows, com o PWA fechado.
 *
 * Não há serviço nosso no meio: o navegador se inscreve no push service dele
 * mesmo (FCM, Mozilla, WNS) e nos dá um endpoint. Nós só assinamos a mensagem
 * com o par VAPID e a entregamos cifrada — nem o push service lê o conteúdo.
 */

const VAPID_PUBLIC = "vapid_public";
const VAPID_PRIVATE = "vapid_private";

/**
 * Depois de tantas recusas seguidas, a inscrição é considerada morta e sai.
 *
 * Não apagamos na primeira: um `500` do push service ou uma queda de rede não
 * deveriam desinscrever um aparelho legítimo. `404` e `410`, esses sim, são o
 * push service dizendo que a inscrição não existe mais — e aí saem na hora.
 */
const MAX_FAILURES = 5;

let configured = false;

/**
 * Garante um par VAPID utilizável e configura a biblioteca.
 *
 * Ordem de preferência: variáveis de ambiente (para quem quer controlar as
 * chaves), senão o par persistido no banco, senão gera um novo e persiste.
 * Gerar automaticamente é o que faz as notificações funcionarem sem ninguém
 * ter de editar o compose; persistir é o que faz as inscrições existentes
 * sobreviverem a um deploy.
 */
export async function ensurePushKeys(rt: Runtime): Promise<string | null> {
  const doEnv = rt.cfg.VAPID_PUBLIC_KEY && rt.cfg.VAPID_PRIVATE_KEY;
  let publicKey: string;
  let privateKey: string;

  if (doEnv) {
    publicKey = rt.cfg.VAPID_PUBLIC_KEY;
    privateKey = rt.cfg.VAPID_PRIVATE_KEY;
  } else {
    const rows = await rt.db.select().from(serverKeys);
    const guardadas = new Map(rows.map((r) => [r.name, r.value]));

    if (guardadas.has(VAPID_PUBLIC) && guardadas.has(VAPID_PRIVATE)) {
      publicKey = guardadas.get(VAPID_PUBLIC)!;
      privateKey = guardadas.get(VAPID_PRIVATE)!;
    } else {
      const par = webpush.generateVAPIDKeys();
      publicKey = par.publicKey;
      privateKey = par.privateKey;

      await rt.db
        .insert(serverKeys)
        .values([
          { name: VAPID_PUBLIC, value: publicKey },
          { name: VAPID_PRIVATE, value: privateKey },
        ])
        .onConflictDoNothing();

      rt.log.info("par VAPID gerado e persistido — notificações push habilitadas");
    }
  }

  // O `mailto:` é exigido pela especificação para o push service ter como
  // contatar o responsável; não precisa ser um endereço monitorado.
  webpush.setVapidDetails(
    `mailto:${rt.cfg.VAPID_CONTACT_EMAIL}`,
    publicKey,
    privateKey,
  );
  configured = true;
  return publicKey;
}

export function pushConfigured(): boolean {
  return configured;
}

export interface PushMessage {
  title: string;
  body: string;
  /** Agrupa notificações do mesmo assunto — a nova substitui a anterior. */
  tag: string;
  /** `true` mantém a notificação até o usuário interagir. Para o que é grave. */
  requireInteraction?: boolean;
}

/**
 * Envia para todas as inscrições ativas.
 *
 * Falha de entrega nunca propaga: uma notificação que não sai não pode
 * derrubar o ingest nem o watchdog, que é de onde este código é chamado.
 */
export async function sendToAll(rt: Runtime, msg: PushMessage): Promise<void> {
  if (!configured) return;

  const inscricoes = await rt.db
    .select()
    .from(pushSubscriptions)
    .where(or(isNull(pushSubscriptions.failedAt), sql`${pushSubscriptions.failureCount} < ${MAX_FAILURES}`));

  if (inscricoes.length === 0) return;

  const payload = JSON.stringify(msg);

  await Promise.all(
    inscricoes.map(async (s) => {
      try {
        await webpush.sendNotification(
          { endpoint: s.endpoint, keys: { p256dh: s.p256dh, auth: s.auth } },
          payload,
        );
        // Sucesso limpa o histórico de falha: um aparelho que voltou não deve
        // carregar para sempre a contagem de quando esteve fora.
        if (s.failureCount > 0) {
          await rt.db
            .update(pushSubscriptions)
            .set({ failedAt: null, failureCount: 0 })
            .where(eq(pushSubscriptions.endpoint, s.endpoint));
        }
      } catch (err) {
        await registrarFalha(rt, s.endpoint, err);
      }
    }),
  );
}

async function registrarFalha(rt: Runtime, endpoint: string, err: unknown): Promise<void> {
  const status = (err as { statusCode?: number }).statusCode;

  // 404/410 são definitivos: o push service afirma que a inscrição não existe
  // mais. Insistir só geraria tráfego e ruído no log.
  if (status === 404 || status === 410) {
    await rt.db.delete(pushSubscriptions).where(eq(pushSubscriptions.endpoint, endpoint));
    rt.log.info({ status }, "inscrição push removida pelo push service");
    return;
  }

  await rt.db
    .update(pushSubscriptions)
    .set({
      failedAt: new Date(),
      failureCount: sql`${pushSubscriptions.failureCount} + 1`,
    })
    .where(eq(pushSubscriptions.endpoint, endpoint));

  rt.log.warn({ err, status }, "falha ao entregar notificação push");
}

/** Registra ou atualiza uma inscrição. Idempotente por `endpoint`. */
export async function subscribe(
  db: Db,
  sub: { endpoint: string; keys: { p256dh: string; auth: string } },
  email: string | null,
): Promise<void> {
  await db
    .insert(pushSubscriptions)
    .values({
      endpoint: sub.endpoint,
      p256dh: sub.keys.p256dh,
      auth: sub.keys.auth,
      createdBy: email,
    })
    .onConflictDoUpdate({
      target: pushSubscriptions.endpoint,
      set: {
        p256dh: sub.keys.p256dh,
        auth: sub.keys.auth,
        createdBy: email,
        failedAt: null,
        failureCount: 0,
      },
    });
}

export async function unsubscribe(db: Db, endpoint: string): Promise<void> {
  await db.delete(pushSubscriptions).where(eq(pushSubscriptions.endpoint, endpoint));
}
