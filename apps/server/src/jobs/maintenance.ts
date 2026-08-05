import { sql } from "drizzle-orm";

import {
  databaseSizeBytes,
  ensureCurrentPartitions,
  enforceSizeLimit,
} from "../db/client.js";
import { events } from "../db/schema.js";
import type { Runtime } from "../runtime.js";

/**
 * Manutenção diária: partições, rollup horário e purga por tamanho.
 *
 * Roda de madrugada de propósito. São 2 núcleos compartilhados com o Postgres,
 * o cloudflared e o resto do homelab — agregação em horário de uso apareceria.
 */
const HOUR = 3;
const MINUTE = 15;
/** Verifica a cada 5 min se a janela chegou. Mais simples que um cron real. */
const CHECK_INTERVAL_MS = 5 * 60 * 1000;

export function startMaintenance(rt: Runtime): () => void {
  let lastRunDay = "";

  const timer = setInterval(() => {
    const now = new Date();
    const day = now.toISOString().slice(0, 10);
    if (day === lastRunDay) return;
    if (now.getUTCHours() < HOUR) return;
    if (now.getUTCHours() === HOUR && now.getUTCMinutes() < MINUTE) return;

    lastRunDay = day;
    void runMaintenance(rt).catch((err) =>
      rt.log.error({ err }, "falha na manutenção diária"),
    );
  }, CHECK_INTERVAL_MS);

  timer.unref();
  return () => clearInterval(timer);
}

export async function runMaintenance(rt: Runtime): Promise<void> {
  if (!rt.isDbReady()) return;

  const started = Date.now();
  rt.log.info("manutenção diária iniciada");

  await ensureCurrentPartitions(rt.db);
  const rolled = await rollupPreviousDay(rt);
  const dropped = await enforceSizeLimit(rt.db, rt.cfg.DB_SIZE_LIMIT_BYTES);

  for (const p of dropped) {
    await rt.db.insert(events).values({
      receivedAt: new Date(),
      deviceId: "aquarium-01",
      source: "server",
      sev: "info",
      comp: "system",
      code: "db.partition_dropped",
      msg: `Partição ${p.partition} descartada para respeitar o teto de tamanho`,
      ctx: { partition: p.partition, freed_bytes: p.freedBytes },
    });
  }

  const size = await databaseSizeBytes(rt.db);

  // O teto não pôde ser respeitado: só restou a partição do mês corrente e ela
  // sozinha excede o limite. Não há o que o job faça — é decisão humana.
  if (size > rt.cfg.DB_SIZE_LIMIT_BYTES) {
    rt.log.warn(
      { size, limit: rt.cfg.DB_SIZE_LIMIT_BYTES },
      "teto de tamanho inatingível",
    );
    await rt.db.insert(events).values({
      receivedAt: new Date(),
      deviceId: "aquarium-01",
      source: "server",
      sev: "warn",
      comp: "system",
      code: "db.size_limit_unreachable",
      msg: "O banco excede o teto e só resta a partição do mês corrente",
      ctx: { size_bytes: size, limit_bytes: rt.cfg.DB_SIZE_LIMIT_BYTES },
    });
  }

  rt.log.info(
    {
      ms: Date.now() - started,
      horas_agregadas: rolled,
      particoes_descartadas: dropped.length,
      tamanho_mb: Math.round(size / 1024 / 1024),
    },
    "manutenção diária concluída",
  );
}

/**
 * Agrega em `telemetry_hourly` as horas do dia anterior.
 *
 * As métricas de tempo são **ponderadas pela duração**, não pela contagem de
 * amostras. Com gravação on-change, uma hora pode ter três linhas ou trinta, e
 * contar linhas diria que a luz ficou acesa "metade das amostras" — número sem
 * significado. Cada amostra vale o intervalo até a seguinte, limitado ao fim
 * da hora para não vazar duração de uma hora para a outra.
 */
async function rollupPreviousDay(rt: Runtime): Promise<number> {
  const result = await rt.db.execute<{ count: string }>(sql`
    with amostras as (
      select
        t.device_id,
        date_trunc('hour', t.received_at) as hour,
        t.light_on,
        t.fan_on,
        t.fan_rpm,
        t.temp_celsius,
        extract(epoch from (
          least(
            coalesce(
              lead(t.received_at) over (
                partition by t.device_id order by t.received_at
              ),
              date_trunc('hour', t.received_at) + interval '1 hour'
            ),
            date_trunc('hour', t.received_at) + interval '1 hour'
          ) - t.received_at
        )) as segundos
      from telemetry t
      where t.received_at >= date_trunc('day', now() - interval '1 day')
        and t.received_at <  date_trunc('day', now())
    ),
    agregado as (
      select
        device_id,
        hour,
        min(temp_celsius)::real                                        as temp_min,
        avg(temp_celsius)::real                                        as temp_avg,
        max(temp_celsius)::real                                        as temp_max,
        -- O coalesce precisa vir DENTRO do least, não fora: no PostgreSQL
        -- least(60, NULL) devolve 60, porque LEAST e GREATEST ignoram nulos.
        -- Com o coalesce do lado de fora, toda hora sem luz seria registrada
        -- como 60 minutos acesa.
        least(60, round(coalesce(sum(segundos) filter (where light_on), 0) / 60.0))::smallint as light_minutes,
        least(60, round(coalesce(sum(segundos) filter (where fan_on),   0) / 60.0))::smallint as fan_minutes,
        round(avg(fan_rpm) filter (where fan_on))::integer             as fan_rpm_avg,
        count(*)::integer                                             as samples
      from amostras
      group by device_id, hour
    )
    insert into telemetry_hourly
      (device_id, hour, temp_min, temp_avg, temp_max,
       light_minutes, fan_minutes, fan_rpm_avg, samples)
    select device_id, hour, temp_min, temp_avg, temp_max,
           light_minutes, fan_minutes, fan_rpm_avg, samples
      from agregado
    on conflict (device_id, hour) do update set
      temp_min      = excluded.temp_min,
      temp_avg      = excluded.temp_avg,
      temp_max      = excluded.temp_max,
      light_minutes = excluded.light_minutes,
      fan_minutes   = excluded.fan_minutes,
      fan_rpm_avg   = excluded.fan_rpm_avg,
      samples       = excluded.samples
  `);

  return result.rowCount ?? 0;
}
