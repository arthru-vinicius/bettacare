import { TEMP_USUAL_C } from "@bettacare/contract";
import { sql } from "drizzle-orm";

import {
  databaseSizeBytes,
  ensureCurrentPartitions,
  enforceSizeLimit,
} from "../db/client.js";
import { devices, events } from "../db/schema.js";
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
  let lastRollupHour = "";

  const timer = setInterval(() => {
    const now = new Date();
    const day = now.toISOString().slice(0, 10);
    const hour = now.toISOString().slice(0, 13);

    if (day !== lastRunDay && maintenanceWindowOpen(now)) {
      lastRunDay = day;
      lastRollupHour = hour; // a diária já agrega as horas pendentes
      void runMaintenance(rt).catch((err) =>
        rt.log.error({ err }, "falha na manutenção diária"),
      );
      return;
    }

    /**
     * Rollup a cada hora cheia, não só de madrugada (UPGRADE/07). Com ele só
     * na manutenção diária, o histórico da interface ficava até um dia
     * atrasado — "hoje" nunca aparecia, e a tela de relatórios parecia não
     * fazer nada. Agregar a hora que acabou de fechar custa uma consulta
     * sobre ~60 linhas.
     */
    if (hour !== lastRollupHour) {
      lastRollupHour = hour;
      void rollupPendingHours(rt).catch((err) =>
        rt.log.error({ err }, "falha no rollup horário"),
      );
    }
  }, CHECK_INTERVAL_MS);

  timer.unref();
  return () => clearInterval(timer);
}

function maintenanceWindowOpen(now: Date): boolean {
  if (now.getUTCHours() < HOUR) return false;
  return !(now.getUTCHours() === HOUR && now.getUTCMinutes() < MINUTE);
}

export async function runMaintenance(rt: Runtime): Promise<void> {
  if (!rt.isDbReady()) return;

  const started = Date.now();
  rt.log.info("manutenção diária iniciada");

  /**
   * Eventos de infraestrutura (purga, teto de tamanho) não pertencem a nenhum
   * dispositivo específico, mas precisam de um `device_id` para aparecer na
   * aba Logs — a interface hoje só consulta o dispositivo padrão. Gravar para
   * **todos os dispositivos cadastrados** é a opção honesta: sem isto, um
   * `"aquarium-01"` fixo perderia o evento em silêncio se o slug real fosse
   * outro (UPGRADE/04, S6).
   */
  const deviceIds = (await rt.db.select({ deviceId: devices.deviceId }).from(devices)).map(
    (d) => d.deviceId,
  );

  await ensureCurrentPartitions(rt.db);
  const rolled = await rollupPendingHours(rt);
  const dropped = await enforceSizeLimit(rt.db, rt.cfg.DB_SIZE_LIMIT_BYTES);

  for (const p of dropped) {
    for (const deviceId of deviceIds) {
      await rt.db.insert(events).values({
        receivedAt: new Date(),
        deviceId,
        source: "server",
        sev: "info",
        comp: "system",
        code: "db.partition_dropped",
        msg: `Partição ${p.partition} descartada para respeitar o teto de tamanho`,
        ctx: { partition: p.partition, freed_bytes: p.freedBytes },
      });
    }
  }

  const size = await databaseSizeBytes(rt.db);

  // O teto não pôde ser respeitado: só restou a partição do mês corrente e ela
  // sozinha excede o limite. Não há o que o job faça — é decisão humana.
  if (size > rt.cfg.DB_SIZE_LIMIT_BYTES) {
    rt.log.warn(
      { size, limit: rt.cfg.DB_SIZE_LIMIT_BYTES },
      "teto de tamanho inatingível",
    );
    for (const deviceId of deviceIds) {
      await rt.db.insert(events).values({
        receivedAt: new Date(),
        deviceId,
        source: "server",
        sev: "warn",
        comp: "system",
        code: "db.size_limit_unreachable",
        msg: "O banco excede o teto e só resta a partição do mês corrente",
        ctx: { size_bytes: size, limit_bytes: rt.cfg.DB_SIZE_LIMIT_BYTES },
      });
    }
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
 * Agrega em `telemetry_hourly` toda hora completa ainda não coberta, por
 * dispositivo — não só "ontem" (UPGRADE/04, S5).
 *
 * A janela fixa original perdia dias inteiros para sempre se o servidor
 * ficasse fora do ar quando a manutenção deveria ter rodado: como
 * `telemetry` é purgada por partição e `telemetry_hourly` é isento, os dias
 * saltados nunca eram agregados, e a telemetria bruta deles acabava purgada
 * antes de alguém perceber. Aqui a cobertura de cada dispositivo é
 * `max(hour)` já agregado até a hora corrente (exclusive — a hora em curso
 * ainda está recebendo amostras). Um piso de 90 dias limita o primeiro run
 * depois de uma lacuna longa, sem impedir a recuperação.
 *
 * As métricas de tempo são **ponderadas pela duração**, não pela contagem de
 * amostras. Com gravação on-change, uma hora pode ter três linhas ou trinta, e
 * contar linhas diria que a luz ficou acesa "metade das amostras" — número sem
 * significado. Cada amostra vale o intervalo até a seguinte, limitado ao fim
 * da hora para não vazar duração de uma hora para a outra.
 */
export async function rollupPendingHours(rt: Runtime): Promise<number> {
  if (!rt.isDbReady()) return 0;
  const result = await rt.db.execute<{ count: string }>(sql`
    with cobertura as (
      select device_id, max(hour) as last_hour
        from telemetry_hourly
       group by device_id
    ),
    janela as (
      select
        d.device_id,
        greatest(
          coalesce(c.last_hour + interval '1 hour', date_trunc('hour', now() - interval '90 days')),
          date_trunc('hour', now() - interval '90 days')
        ) as from_hour,
        date_trunc('hour', now()) as to_hour
      from devices d
      left join cobertura c on c.device_id = d.device_id
    ),
    amostras as (
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
      join janela j
        on j.device_id = t.device_id
       and t.received_at >= j.from_hour
       and t.received_at <  j.to_hour
    ),
    agregado as (
      select
        device_id,
        hour,
        -- Só a faixa habitual da água (TEMP_USUAL_C, no contrato): o gráfico
        -- mostra o comportamento normal, e o que saiu dela vira aviso nos
        -- Registros, com duração e pico (temp.out_of_usual_range). Um -48
        -- corrompido que passou no CRC puxava a média da hora 1,5 °C para
        -- baixo e esticava o eixo do gráfico inteiro.
        (min(temp_celsius) filter (where temp_celsius between ${TEMP_USUAL_C.min} and ${TEMP_USUAL_C.max}))::real as temp_min,
        (avg(temp_celsius) filter (where temp_celsius between ${TEMP_USUAL_C.min} and ${TEMP_USUAL_C.max}))::real as temp_avg,
        (max(temp_celsius) filter (where temp_celsius between ${TEMP_USUAL_C.min} and ${TEMP_USUAL_C.max}))::real as temp_max,
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
