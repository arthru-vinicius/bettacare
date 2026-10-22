import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { after, before, beforeEach, describe, it } from "node:test";
import { sanitizeTelemetryRequest } from "@bettacare/contract";
import { eq, sql } from "drizzle-orm";

import { getFanHistory } from "../src/api/history.js";
import { loadConfig } from "../src/config.js";
import { createDb, createPool, MIGRATIONS_FOLDER, runMigrations } from "../src/db/client.js";
import { fanRuns } from "../src/db/schema.js";
import { processTelemetry } from "../src/ingest/process.js";
import { pruneFanRuns } from "../src/jobs/maintenance.js";
import { createLogger } from "../src/logger.js";
import { createRuntime } from "../src/runtime.js";

/**
 * Histórico da ventoinha (v1.4.0): as rodadas gravadas pelo ingest, a série de
 * temperatura da API, a poda por idade e a reconstrução da migration 0013.
 *
 * Precisa de Postgres de verdade — ver `rollup.test.ts` para como rodar.
 */
const url = process.env["DATABASE_URL"];
const DEV = "teste-acionamentos";
const DEV_HIST = "teste-historico";
const DEV_BACKFILL = "teste-backfill";

function corpo(over: Record<string, unknown> = {}) {
  return {
    device_id: DEV,
    boot_id: 10,
    seq: 1,
    uptime_ms: 3_600_000,
    config_version: 0,
    light: { on: false, source: "schedule" },
    temperature: { celsius: 28.0, available: true, valid: true, age_ms: 800 },
    fan: { on: false, speed_percent: 0, rpm: 0, mode: "auto" },
    rtc: { available: true, time: "14:30", lost_power: false },
    wifi: { rssi: -62, ip: "192.168.100.46", reconnects: 0 },
    ...over,
  };
}

const ligada = (pct = 30, mode = "auto") => ({
  on: true,
  speed_percent: pct,
  rpm: pct * 40,
  mode,
});
const temp = (celsius: number) => ({ celsius, available: true, valid: true, age_ms: 800 });

describe(
  "histórico da ventoinha: rodadas, série de temperatura e poda",
  { skip: url ? false : "DATABASE_URL não definida" },
  () => {
    let rt: ReturnType<typeof createRuntime>;
    let seq = 1;
    /** Instante-base: 2 h atrás, para a rodada caber na janela de 24 h da API. */
    let base = new Date();

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

    beforeEach(async () => {
      await limpar();
      seq = 1;
      base = new Date(Date.now() - 2 * 3600_000);
    });

    after(async () => {
      await limpar();
      await rt.pool.end();
    });

    async function limpar() {
      for (const d of [DEV, DEV_HIST, DEV_BACKFILL]) {
        for (const t of ["telemetry", "events", "component_status", "settings", "device_state"]) {
          await rt.db.execute(sql`delete from ${sql.identifier(t)} where device_id = ${d}`);
        }
        // `fan_runs` sai junto, em cascata.
        await rt.db.execute(sql`delete from devices where device_id = ${d}`);
      }
    }

    async function post(segundos: number, over: Record<string, unknown> = {}) {
      const s = sanitizeTelemetryRequest(corpo({ seq: seq++, ...over }));
      assert.notEqual(s.data, null);
      const quando = new Date(base.getTime() + segundos * 1000);
      return processTelemetry(rt.db, rt.cfg, s.data!, quando, s.corrections);
    }

    async function rodadas(device = DEV) {
      return rt.db.select().from(fanRuns).where(eq(fanRuns.deviceId, device)).orderBy(fanRuns.startedAt);
    }

    it("a rodada abre ao ligar, acompanha o pico e fecha com a temperatura de saída", async () => {
      await post(0);
      assert.equal((await rodadas()).length, 0, "desligada não abre nada");

      await post(10, { fan: ligada(30), temperature: temp(29.1) });
      let [r] = await rodadas();
      assert.ok(r);
      assert.equal(r.endedAt, null, "aberta enquanto gira");
      assert.equal(r.tempStart, 29.1);
      assert.equal(r.mode, "auto");
      assert.equal(r.manual, false);
      assert.equal(r.approx, false);
      assert.equal(r.startedAt.getTime(), base.getTime() + 10_000);

      // A temperatura sobe 0,3 °C e a potência dobra: duas mudanças, duas gravações.
      await post(30, { fan: ligada(30), temperature: temp(29.4) });
      await post(250, { fan: ligada(60), temperature: temp(29.0) });
      [r] = await rodadas();
      assert.equal(r!.tempPeak, 29.4, "o pico é o maior valor visto girando");
      assert.equal(r!.tempLow, 29.0);
      assert.equal(r!.speedMax, 60);

      await post(500, { fan: { on: false, speed_percent: 0, rpm: 0, mode: "auto" }, temperature: temp(27.4) });
      const todas = await rodadas();
      assert.equal(todas.length, 1, "uma rodada só, não uma por POST");
      r = todas[0];
      assert.equal(r!.endedAt?.getTime(), base.getTime() + 500_000);
      assert.equal(r!.tempEnd, 27.4);
      assert.equal(r!.approx, false);
    });

    it("alguém mexeu durante a rodada: ela fica marcada como manual", async () => {
      await post(0);
      await post(10, { fan: ligada(30), temperature: temp(29.1) });
      await post(40, { fan: ligada(100, "manual"), temperature: temp(29.1) });
      const [r] = await rodadas();
      assert.equal(r!.manual, true);
      assert.equal(r!.mode, "auto", "como começou não muda");
      assert.equal(r!.speedMax, 100);
    });

    it("silêncio no meio da rodada: o fim é a última vez que se viu ligada, sem temperatura de saída", async () => {
      await post(0);
      await post(10, { fan: ligada(30), temperature: temp(29.1) });
      await post(75, { fan: ligada(30), temperature: temp(29.2) }); // heartbeat de 60 s
      // Sumiu por quase uma hora e voltou com a ventoinha desligada.
      await post(3600, { fan: { on: false, speed_percent: 0, rpm: 0, mode: "auto" }, temperature: temp(27.0) });
      const [r] = await rodadas();
      assert.equal(r!.endedAt?.getTime(), base.getTime() + 75_000, "limite inferior, não 'agora'");
      assert.equal(r!.tempEnd, null, "a temperatura de depois do silêncio não é a de saída");
      assert.equal(r!.approx, true);
    });

    it("reiniciar ligada fecha a rodada no reinício e abre outra", async () => {
      await post(0);
      await post(10, { fan: ligada(30), temperature: temp(29.1) });
      await post(100, { boot_id: 11, seq: 1, uptime_ms: 20_000, fan: ligada(30), temperature: temp(29.0) });
      const todas = await rodadas();
      assert.equal(todas.length, 2);
      assert.equal(todas[0]!.endedAt?.getTime(), base.getTime() + 80_000, "100 s - 20 s de uptime");
      assert.equal(todas[1]!.endedAt, null);
    });

    it("no máximo uma rodada aberta por aparelho, mesmo se o fechamento tiver se perdido", async () => {
      await post(0);
      await post(10, { fan: ligada(30) });
      // Fecha na mão sem passar pelo ingest, como um fechamento perdido, e liga de novo.
      await rt.db.execute(sql`delete from fan_runs where device_id = ${DEV}`);
      await post(80, { fan: ligada(30) }); // heartbeat de 60 s: é o que toca a rodada
      const todas = await rodadas();
      assert.equal(todas.length, 1, "sem rodada aberta, a próxima leitura ligada abre uma, aproximada");
      assert.equal(todas[0]!.approx, true);
    });

    it("a API devolve a série de temperatura, os extremos e as rodadas do período", async () => {
      await rt.db.execute(sql`insert into devices (device_id) values (${DEV_HIST}) on conflict do nothing`);
      // 3 h a 1 h atrás, uma linha por minuto, menos 20 min sem nenhuma (o aquário mudo).
      await rt.db.execute(sql`
        insert into telemetry
          (received_at, device_id, boot_id, seq, light_on, temp_celsius, temp_valid,
           fan_on, fan_speed_percent, fan_rpm, fan_mode)
        select date_trunc('minute', now()) - (g || ' minutes')::interval,
               ${DEV_HIST}, 1, g, false, 27.0 + (g % 10) * 0.1, true, false, 0, 0, 'auto'
          from generate_series(60, 180) g
         where g not between 110 and 130
      `);
      // Uma leitura velha repetida (temp_valid falso) não é medição e não entra.
      await rt.db.execute(sql`
        insert into telemetry
          (received_at, device_id, boot_id, seq, light_on, temp_celsius, temp_valid,
           fan_on, fan_speed_percent, fan_rpm, fan_mode)
        values (date_trunc('minute', now()) - interval '70 minutes' + interval '30 seconds',
                ${DEV_HIST}, 1, 9999, false, 40.0, false, false, 0, 0, 'auto')
      `);
      await rt.db.execute(sql`
        insert into fan_runs (device_id, started_at, ended_at, last_on_at, temp_start, temp_end, temp_peak, temp_low)
        values (${DEV_HIST}, now() - interval '90 minutes', now() - interval '60 minutes',
                now() - interval '61 minutes', 29.2, 27.6, 29.4, 27.5)
      `);

      const h = await getFanHistory(rt.db, DEV_HIST, "24h");
      assert.equal(h.bucket_s, 60);
      assert.equal(h.points.length, 100, "121 minutos de leitura menos os 21 sem nenhuma");
      for (const [t, avg, min, max] of h.points) {
        assert.equal(t % 60, 0, "cada ponto cai numa fronteira de minuto");
        assert.ok(min <= avg && avg <= max);
        assert.ok(max <= 27.9 + 1e-9, "a leitura de 40 °C com temp_valid falso ficou de fora");
      }
      assert.equal(h.temperature?.min, 27);
      assert.equal(h.temperature?.max, 27.9);
      // O mínimo (27,0) aconteceu pela primeira vez há 3 h, o máximo (27,9) um minuto depois.
      const haMin = (iso: string) => (Date.now() - Date.parse(iso)) / 60_000;
      assert.ok(Math.abs(haMin(h.temperature!.min_at) - 180) < 2, "min_at ≈ 3 h atrás");
      assert.ok(Math.abs(haMin(h.temperature!.max_at) - 179) < 2, "max_at ≈ 2 h 59 min atrás");

      assert.equal(h.runs.length, 1);
      assert.equal(h.runs[0]!.duration_s, 1800);
      assert.equal(h.runs[0]!.temp_start, 29.2);
      assert.equal(h.runs[0]!.temp_end, 27.6);
      assert.equal(h.summary.runs, 1);
      assert.equal(h.summary.on_seconds, 1800);
      assert.equal(h.summary.avg_duration_s, 1800);
      assert.equal(h.summary.avg_drop_c, 1.6);

      // Período curto demais para a rodada: sem ela, e sem ponto nenhum.
      const longe = await getFanHistory(rt.db, DEV_HIST, "24h", new Date(Date.now() - 20 * 3600_000));
      assert.equal(longe.runs.length, 0);
      assert.equal(longe.points.length, 0);
      assert.equal(longe.temperature, null);
    });

    it("a poda apaga o antigo, respeita o teto por aparelho e nunca toca a rodada aberta", async () => {
      await rt.db.execute(sql`insert into devices (device_id) values (${DEV_HIST}) on conflict do nothing`);
      const rodada = (diasAtras: number, aberta = false) => sql`
        insert into fan_runs (device_id, started_at, ended_at, last_on_at)
        values (${DEV_HIST}, now() - make_interval(days => ${diasAtras}::int),
                ${aberta ? null : sql`now() - make_interval(days => ${diasAtras}::int) + interval '10 minutes'`},
                now() - make_interval(days => ${diasAtras}::int))
      `;
      await rt.db.execute(rodada(200));
      await rt.db.execute(rodada(95));
      await rt.db.execute(rodada(80));
      await rt.db.execute(rodada(10));
      await rt.db.execute(rodada(1));

      assert.equal(await pruneFanRuns(rt.db, 90, 20_000), 2, "200 e 95 dias passam dos 90");
      assert.equal((await rodadas(DEV_HIST)).length, 3);

      assert.equal(await pruneFanRuns(rt.db, 90, 2), 1, "teto de 2: sai a mais antiga das 3");
      const restantes = await rodadas(DEV_HIST);
      assert.equal(restantes.length, 2);

      // Rodada aberta antiga demais: fica, porque é a ventoinha girando agora.
      await rt.db.execute(sql`delete from fan_runs where device_id = ${DEV_HIST}`);
      await rt.db.execute(rodada(400, true));
      assert.equal(await pruneFanRuns(rt.db, 90, 1), 0);
      assert.equal((await rodadas(DEV_HIST)).length, 1);
    });

    it("a migration 0013 reconstrói as rodadas que já estavam na telemetria", async () => {
      await rt.db.execute(sql`insert into devices (device_id) values (${DEV_BACKFILL}) on conflict do nothing`);
      // `telemetry` é particionada por mês, e só existem a partição do mês corrente e a seguinte:
      // as linhas ficam no começo do mês corrente.
      const linha = (minuto: number, ligada: boolean, pct: number, modo: string, celsius: number) => sql`
        (date_trunc('month', now()) + interval '2 days 12 hours' + make_interval(mins => ${minuto}),
         ${DEV_BACKFILL}, 1, ${minuto}, false, ${celsius}, true, ${ligada}, ${pct}, ${pct * 40}, ${modo}::fan_mode)
      `;
      const linhas = [
        linha(0, false, 0, "auto", 25.0),
        linha(1, true, 30, "auto", 29.1), // A liga
        linha(2, true, 30, "auto", 29.4),
        linha(3, true, 60, "auto", 29.0),
        linha(5, true, 60, "auto", 28.5),
        linha(7, true, 60, "auto", 28.0),
        linha(9, true, 60, "auto", 27.8),
        linha(10, false, 0, "auto", 27.4), // A desliga
        linha(30, false, 0, "auto", 27.0),
        linha(40, true, 100, "manual", 26.0), // B liga, manual
        linha(41, true, 100, "manual", 26.0),
        linha(80, false, 0, "auto", 26.0), // 39 min de silêncio até a linha que registra o fim
        linha(90, false, 0, "auto", 26.0),
        linha(100, true, 30, "auto", 29.5), // C liga e segue ligada
      ];
      await rt.db.execute(sql`
        insert into telemetry
          (received_at, device_id, boot_id, seq, light_on, temp_celsius, temp_valid,
           fan_on, fan_speed_percent, fan_rpm, fan_mode)
        values ${sql.join(linhas, sql`, `)}
      `);

      const arquivo = readFileSync(join(MIGRATIONS_FOLDER, "0013_acionamentos_ventoinha.sql"), "utf8");
      const comandos = arquivo.split("--> statement-breakpoint").map((c) => c.trim());
      const reconstrucao = comandos.at(-1)!;
      assert.match(reconstrucao, /INSERT INTO "fan_runs"/);
      await rt.db.execute(sql.raw(reconstrucao));

      const [a, b, c] = await rodadas(DEV_BACKFILL);
      assert.equal((await rodadas(DEV_BACKFILL)).length, 3);

      const inicio = (min: number) =>
        rt.db
          .execute<{ ms: number }>(
            sql`select (extract(epoch from date_trunc('month', now()) + interval '2 days 12 hours' + make_interval(mins => ${min})) * 1000)::float8 as ms`,
          )
          .then((r) => r.rows[0]!.ms);

      assert.equal(a!.startedAt.getTime(), await inicio(1));
      assert.equal(a!.endedAt?.getTime(), await inicio(10), "A fecha na linha que registra o desligamento");
      assert.equal(a!.tempStart, 29.1);
      assert.equal(a!.tempEnd, 27.4);
      assert.equal(a!.tempPeak, 29.4);
      assert.equal(a!.tempLow, 27.8);
      assert.equal(a!.speedMax, 60);
      assert.equal(a!.manual, false);
      assert.equal(a!.approx, false);

      assert.equal(b!.mode, "manual");
      assert.equal(b!.manual, true);
      assert.equal(b!.endedAt?.getTime(), await inicio(41), "39 min de silêncio: o fim é a última linha ligada");
      assert.equal(b!.tempEnd, null);
      assert.equal(b!.approx, true);

      assert.equal(c!.endedAt, null, "ainda girando no fim da telemetria");
      assert.equal(c!.approx, true, "10 min de silêncio antes de ligar");
    });
  },
);
