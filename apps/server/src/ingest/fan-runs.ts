import { and, eq, isNull, sql } from "drizzle-orm";

import type { Db } from "../db/client.js";
import { fanRuns } from "../db/schema.js";

/**
 * Rodadas da ventoinha (`fan_runs`): do instante em que liga ao instante em que
 * desliga, com a temperatura de partida, o pico e a de saída.
 *
 * O ingest acompanha a transição `fan.on` POST a POST. A decisão — abrir,
 * fechar, continuar — é uma função pura (`planFanRun`), testável sem banco; o
 * resto só a executa.
 */

type Tx = Parameters<Parameters<Db["transaction"]>[0]>[0];

/**
 * Silêncio a partir do qual o início ou o fim de uma rodada deixa de ser
 * exato. O aquário fala a cada 1–5 s; mais que isso é uma queda de rede, e o
 * que aconteceu nela ninguém viu — a ventoinha pode ter ligado, desligado e
 * ligado de novo sem deixar rastro.
 */
export const RUN_GAP_MS = 5 * 60_000;

export type FanMode = "auto" | "manual" | "manual_off" | "failsafe";

/** O que este POST diz sobre a ventoinha. */
export interface FanObservation {
  on: boolean;
  mode: FanMode;
  speedPercent: number;
  /** Só leitura fresca e válida: uma temperatura velha não é a de agora. */
  celsius: number | null;
  bootId: number;
  uptimeMs: number | null;
  at: Date;
}

/** O que o POST anterior (o último aceito) dizia. Nulo no primeiro contato. */
export interface FanPrevious {
  on: boolean;
  at: Date;
  bootId: number;
}

export type RunStep =
  | { kind: "none" }
  /** `approx`: ligou em algum momento do silêncio; `startedAt` é o primeiro POST que viu. */
  | { kind: "open"; startedAt: Date; approx: boolean }
  /**
   * `useLastOn`: houve silêncio, então o fim é a última vez que se viu a
   * ventoinha ligada (um limite inferior) e a temperatura de saída é
   * desconhecida — a de agora já é de depois.
   */
  | { kind: "close"; useLastOn: boolean }
  | { kind: "continue"; approx: boolean }
  /** Reiniciou ligada: a rodada velha acabou no reinício, outra começa. */
  | { kind: "reopen"; closeAt: Date | null; startedAt: Date };

export function planFanRun(prev: FanPrevious | null, obs: FanObservation): RunStep {
  const wasOn = prev?.on ?? false;
  if (!wasOn && !obs.on) return { kind: "none" };

  const gap = prev === null || obs.at.getTime() - prev.at.getTime() > RUN_GAP_MS;

  if (!wasOn) return { kind: "open", startedAt: obs.at, approx: gap };
  if (!obs.on) return { kind: "close", useLastOn: gap };

  // Ligada antes e agora.
  if (prev!.bootId === obs.bootId) return { kind: "continue", approx: gap };

  // O ESP32 reiniciou com a ventoinha ligada: o PWM zera no reset, então a
  // rodada anterior terminou ali. O instante do reinício é `uptime` atrás.
  if (gap) return { kind: "reopen", closeAt: null, startedAt: obs.at };
  const bootAt = obs.uptimeMs === null ? obs.at : new Date(obs.at.getTime() - obs.uptimeMs);
  const closeAt = new Date(
    Math.min(obs.at.getTime(), Math.max(prev!.at.getTime(), bootAt.getTime())),
  );
  return { kind: "reopen", closeAt, startedAt: obs.at };
}

const isManual = (mode: FanMode) => mode === "manual" || mode === "manual_off";

/**
 * Aplica a transição desta leva de telemetria.
 *
 * `wroteHistory` limita o custo da rodada em curso: o POST chega a cada 1 s,
 * mas só a cada mudança ou minuto de heartbeat vale uma gravação — o suficiente
 * para pico, potência e "alguém mexeu" sem uma escrita por segundo.
 */
export async function trackFanRun(
  tx: Tx,
  deviceId: string,
  prev: FanPrevious | null,
  obs: FanObservation,
  wroteHistory: boolean,
): Promise<void> {
  const step = planFanRun(prev, obs);

  switch (step.kind) {
    case "none":
      return;

    case "open":
      // Uma rodada aberta que sobrou (o fechamento se perdeu) não pode
      // atrapalhar a nova: o índice parcial só admite uma por aparelho.
      await closeRun(tx, deviceId, obs, { useLastOn: true, closeAt: null });
      await insertRun(tx, deviceId, obs, step.startedAt, step.approx);
      return;

    case "close":
      await closeRun(tx, deviceId, obs, { useLastOn: step.useLastOn, closeAt: null });
      return;

    case "reopen":
      await closeRun(tx, deviceId, obs, {
        useLastOn: step.closeAt === null,
        closeAt: step.closeAt,
      });
      await insertRun(tx, deviceId, obs, step.startedAt, false);
      return;

    case "continue":
      if (wroteHistory || step.approx) await touchRun(tx, deviceId, obs, step.approx);
      return;
  }
}

async function insertRun(
  tx: Tx,
  deviceId: string,
  obs: FanObservation,
  startedAt: Date,
  approx: boolean,
): Promise<void> {
  await tx
    .insert(fanRuns)
    .values({
      deviceId,
      startedAt,
      lastOnAt: obs.at,
      mode: obs.mode === "manual_off" ? "manual" : obs.mode,
      manual: isManual(obs.mode),
      speedMax: obs.speedPercent,
      tempStart: obs.celsius,
      tempPeak: obs.celsius,
      tempLow: obs.celsius,
      approx,
    })
    .onConflictDoNothing();
}

async function closeRun(
  tx: Tx,
  deviceId: string,
  obs: FanObservation,
  how: { useLastOn: boolean; closeAt: Date | null },
): Promise<void> {
  await tx
    .update(fanRuns)
    .set({
      endedAt: how.useLastOn && how.closeAt === null ? sql`${fanRuns.lastOnAt}` : (how.closeAt ?? obs.at),
      // Fim sem testemunha: a temperatura de agora não é a da saída.
      tempEnd: how.useLastOn ? null : obs.celsius,
      ...(how.useLastOn ? { approx: true } : {}),
    })
    .where(and(eq(fanRuns.deviceId, deviceId), isNull(fanRuns.endedAt)));
}

async function touchRun(
  tx: Tx,
  deviceId: string,
  obs: FanObservation,
  approx: boolean,
): Promise<void> {
  const c = obs.celsius;
  const updated = await tx
    .update(fanRuns)
    .set({
      lastOnAt: obs.at,
      speedMax: sql`greatest(${fanRuns.speedMax}, ${obs.speedPercent}::smallint)`,
      ...(c === null
        ? {}
        : {
            tempPeak: sql`greatest(coalesce(${fanRuns.tempPeak}, ${c}::real), ${c}::real)`,
            tempLow: sql`least(coalesce(${fanRuns.tempLow}, ${c}::real), ${c}::real)`,
          }),
      ...(isManual(obs.mode) ? { manual: true } : {}),
      ...(approx ? { approx: true } : {}),
    })
    .where(and(eq(fanRuns.deviceId, deviceId), isNull(fanRuns.endedAt)))
    .returning({ id: fanRuns.id });

  // Ligada e sem rodada aberta: o início se perdeu (a tabela é mais nova que a
  // ventoinha, ou o fechamento anterior falhou). Melhor uma rodada aproximada
  // do que nenhuma.
  if (updated.length === 0) await insertRun(tx, deviceId, obs, obs.at, true);
}
