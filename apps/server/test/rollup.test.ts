import assert from "node:assert/strict";
import { after, before, describe, it } from "node:test";
import { sql } from "drizzle-orm";

import { loadConfig } from "../src/config.js";
import { createDb, createPool, runMigrations } from "../src/db/client.js";
import { runMaintenance } from "../src/jobs/maintenance.js";
import { createLogger } from "../src/logger.js";
import { createRuntime } from "../src/runtime.js";

/**
 * Teste de integração do rollup horário. Precisa de um Postgres de verdade:
 *
 *   pnpm db:up
 *   DATABASE_URL=postgresql://bettacare_user:bettacare_dev@127.0.0.1:5433/bettacare \
 *     pnpm --filter @bettacare/server test
 *
 * Sem `DATABASE_URL` o arquivo inteiro é pulado, para o `turbo test` continuar
 * rodando em máquina sem Docker.
 */
const url = process.env["DATABASE_URL"];

describe("rollup horário", { skip: url ? false : "DATABASE_URL não definida" }, () => {
  let rt: ReturnType<typeof createRuntime>;

  before(async () => {
    const cfg = loadConfig({
      DATABASE_URL: url,
      DEVICE_INGEST_TOKEN: "token-de-teste-com-tamanho-ok",
      LOG_LEVEL: "fatal",
      NODE_ENV: "test",
    });
    const pool = createPool(cfg.DATABASE_URL, 2);
    rt = createRuntime({ cfg, db: createDb(pool), pool, log: createLogger(cfg) });
    await runMigrations(rt.db);
    rt.setDbReady(true);
  });

  after(async () => {
    await rt.pool.end();
  });

  it("pondera pela duração e não confunde ausência com hora cheia", async () => {
    // Um dia inteiro de ontem, amostra a cada 5 min. Luz das 10h às 17h
    // (7 horas), ventoinha das 13h às 15h (2 horas).
    await rt.db.execute(sql`
      delete from telemetry
       where device_id = 'teste-rollup'
         and received_at >= date_trunc('day', now() - interval '1 day')
         and received_at <  date_trunc('day', now());
    `);
    await rt.db.execute(sql`delete from telemetry_hourly where device_id = 'teste-rollup'`);
    await rt.db.execute(sql`
      insert into devices (device_id) values ('teste-rollup')
      on conflict do nothing
    `);
    await rt.db.execute(sql`
      insert into telemetry
        (received_at, device_id, boot_id, seq, light_on, temp_celsius, temp_valid,
         fan_on, fan_speed_percent, fan_rpm, fan_mode)
      select
        date_trunc('day', now() - interval '1 day') + (g || ' minutes')::interval,
        'teste-rollup', 1, g,
        (g/60 >= 10 and g/60 < 17),
        26.0, true,
        (g/60 >= 13 and g/60 < 15),
        case when (g/60 >= 13 and g/60 < 15) then 60 else 0 end,
        case when (g/60 >= 13 and g/60 < 15) then 1380 else 0 end,
        'auto'
      from generate_series(0, 1439, 5) g
    `);

    await runMaintenance(rt);

    const r = await rt.db.execute<{
      total_luz: string;
      total_fan: string;
      horas_zeradas: string;
    }>(sql`
      select coalesce(sum(light_minutes), 0)::text as total_luz,
             coalesce(sum(fan_minutes), 0)::text   as total_fan,
             count(*) filter (where light_minutes = 0)::text as horas_zeradas
        from telemetry_hourly
       where device_id = 'teste-rollup'
    `);

    const row = r.rows[0]!;

    assert.equal(row.total_luz, "420", "7 horas de luz = 420 minutos");
    assert.equal(row.total_fan, "120", "2 horas de ventoinha = 120 minutos");
    /**
     * A regressão específica: `least(60, NULL)` devolve **60** no PostgreSQL,
     * porque LEAST ignora nulos. Com o `coalesce` do lado de fora do `least`,
     * toda hora sem luz era gravada como 60 minutos acesa — e o relatório
     * ficava errado sem nenhum erro aparecer em lugar nenhum.
     */
    assert.equal(row.horas_zeradas, "17", "as 17 horas sem luz precisam ser zero");
  });
});
