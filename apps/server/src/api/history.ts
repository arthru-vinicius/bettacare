import {
  TEMP_PLAUSIBLE_C,
  type FanHistoryResponse,
  type FanRun,
  type HistoryRange,
  type TemperaturePoint,
} from "@bettacare/contract";
import { and, count, desc, eq, gte, isNull, lt, or, sql } from "drizzle-orm";

import type { Db } from "../db/client.js";
import { devices, fanRuns } from "../db/schema.js";

/**
 * Resolução de cada período: até ~1.400 pontos por resposta.
 *
 * - 24 h a 1 min: a telemetria grava ao menos uma linha por minuto (heartbeat),
 *   então é praticamente a leitura de cada instante — "27,8 °C às 14:35".
 * - 7 dias a 15 min e 30 dias a 1 h: mais fino que um dedo distingue numa tela
 *   de celular (1 px = 30 min e 2 h), e o ponto traz mínima e máxima, então um
 *   pico curto não some na média.
 *
 * `gapS` é o silêncio que interrompe a linha: um intervalo e meio vazio entre
 * dois pontos. Ligar os dois por uma reta desenharia uma temperatura que
 * ninguém mediu.
 */
export const HISTORY_SPEC: Record<
  HistoryRange,
  { seconds: number; bucketS: number; gapS: number }
> = {
  "24h": { seconds: 24 * 3600, bucketS: 60, gapS: 150 },
  "7d": { seconds: 7 * 24 * 3600, bucketS: 900, gapS: 2250 },
  "30d": { seconds: 30 * 24 * 3600, bucketS: 3600, gapS: 9000 },
};

/** Teto de rodadas por resposta — uma ventoinha oscilando não pode inflar o corpo. */
export const RUNS_LIMIT = 500;

const round2 = (x: number) => Math.round(x * 100) / 100;
const orNull = (x: number | null): number | null => (x === null ? null : round2(x));

/**
 * Série de temperatura + rodadas da ventoinha de um período.
 *
 * Só entram leituras frescas e plausíveis (`temp_valid`, 10–45 °C): uma leitura
 * velha repetida não é medição do instante, e o ingest já troca a impossível
 * pela última boa antes de gravar.
 */
export async function getFanHistory(
  db: Db,
  deviceId: string,
  range: HistoryRange,
  now: Date = new Date(),
): Promise<FanHistoryResponse> {
  const spec = HISTORY_SPEC[range];
  // O começo cai numa fronteira de intervalo, para o primeiro ponto ser inteiro.
  const fromSec = Math.floor(now.getTime() / 1000 - spec.seconds);
  const from = new Date(Math.floor(fromSec / spec.bucketS) * spec.bucketS * 1000);
  const fromIso = from.toISOString();
  const toIso = now.toISOString();

  const pontos = await db.execute<{ t: number; avg: number; min: number; max: number }>(sql`
    select (floor(extract(epoch from received_at) / ${spec.bucketS}::int) * ${spec.bucketS}::int)::float8 as t,
           round(avg(temp_celsius)::numeric, 2)::float8 as avg,
           round(min(temp_celsius)::numeric, 2)::float8 as min,
           round(max(temp_celsius)::numeric, 2)::float8 as max
      from telemetry
     where device_id = ${deviceId}
       and received_at >= ${fromIso}::timestamptz
       and received_at <  ${toIso}::timestamptz
       and temp_valid
       and temp_celsius between ${TEMP_PLAUSIBLE_C.min} and ${TEMP_PLAUSIBLE_C.max}
     group by 1
     order by 1
  `);

  // Extremos exatos (e o instante da primeira vez que ocorreram), não os do
  // intervalo agregado: "máxima 28,8 às 14:35" tem de bater com a linha.
  const extremos = await db.execute<{
    min: number | null;
    min_at: number | null;
    avg: number | null;
    max: number | null;
    max_at: number | null;
  }>(sql`
    select round(min(temp_celsius)::numeric, 2)::float8 as min,
           (array_agg(extract(epoch from received_at) order by temp_celsius asc,  received_at asc))[1]::float8 as min_at,
           round(avg(temp_celsius)::numeric, 2)::float8 as avg,
           round(max(temp_celsius)::numeric, 2)::float8 as max,
           (array_agg(extract(epoch from received_at) order by temp_celsius desc, received_at asc))[1]::float8 as max_at
      from telemetry
     where device_id = ${deviceId}
       and received_at >= ${fromIso}::timestamptz
       and received_at <  ${toIso}::timestamptz
       and temp_valid
       and temp_celsius between ${TEMP_PLAUSIBLE_C.min} and ${TEMP_PLAUSIBLE_C.max}
  `);

  const [dispositivo] = await db
    .select({ online: devices.online })
    .from(devices)
    .where(eq(devices.deviceId, deviceId))
    .limit(1);
  const online = dispositivo?.online ?? false;

  // Toca o período: começou antes do fim dele e não terminou antes do começo.
  const noPeriodo = and(
    eq(fanRuns.deviceId, deviceId),
    lt(fanRuns.startedAt, now),
    or(isNull(fanRuns.endedAt), gte(fanRuns.endedAt, from)),
  );

  const linhas = await db
    .select()
    .from(fanRuns)
    .where(noPeriodo)
    .orderBy(desc(fanRuns.startedAt))
    .limit(RUNS_LIMIT);
  const [{ total } = { total: 0 }] = await db
    .select({ total: count() })
    .from(fanRuns)
    .where(noPeriodo);

  const runs: FanRun[] = linhas.map((r) => {
    // Rodada aberta: até agora, se o aquário está falando; senão, até a última
    // vez que se viu a ventoinha ligada — não dá para afirmar mais que isso.
    const fim = r.endedAt ?? (online ? now : r.lastOnAt);
    return {
      id: r.id,
      started_at: r.startedAt.toISOString(),
      ended_at: r.endedAt?.toISOString() ?? null,
      duration_s: Math.max(0, Math.round((fim.getTime() - r.startedAt.getTime()) / 1000)),
      mode: r.mode === "manual_off" ? "manual" : r.mode,
      manual: r.manual,
      speed_max: r.speedMax,
      temp_start: orNull(r.tempStart),
      temp_end: orNull(r.tempEnd),
      temp_peak: orNull(r.tempPeak),
      temp_low: orNull(r.tempLow),
      approx: r.approx,
    };
  });

  const noJanela = (r: FanRun): number => {
    const ini = Math.max(Date.parse(r.started_at), from.getTime());
    const fim = Math.min(Date.parse(r.started_at) + r.duration_s * 1000, now.getTime());
    return Math.max(0, fim - ini) / 1000;
  };
  const encerradas = runs.filter((r) => r.ended_at !== null);
  const quedas = encerradas
    .filter((r) => !r.manual && r.temp_start !== null && r.temp_end !== null)
    .map((r) => (r.temp_start as number) - (r.temp_end as number));
  const media = (xs: number[]) => xs.reduce((a, b) => a + b, 0) / xs.length;

  const e = extremos.rows[0];
  const temperatura =
    e && e.min !== null && e.max !== null && e.avg !== null && e.min_at !== null && e.max_at !== null
      ? {
          min: e.min,
          min_at: new Date(e.min_at * 1000).toISOString(),
          avg: e.avg,
          max: e.max,
          max_at: new Date(e.max_at * 1000).toISOString(),
        }
      : null;

  return {
    range,
    from: fromIso,
    to: toIso,
    bucket_s: spec.bucketS,
    gap_s: spec.gapS,
    points: pontos.rows.map((p): TemperaturePoint => [p.t, p.avg, p.min, p.max]),
    temperature: temperatura,
    runs,
    runs_total: total,
    summary: {
      runs: total,
      on_seconds: Math.round(runs.reduce((a, r) => a + noJanela(r), 0)),
      avg_duration_s: encerradas.length
        ? Math.round(media(encerradas.map((r) => r.duration_s)))
        : null,
      avg_drop_c: quedas.length ? round2(media(quedas)) : null,
    },
  };
}
