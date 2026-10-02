import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { after, before, describe, it } from "node:test";
import { sanitizeTelemetryRequest } from "@bettacare/contract";
import { and, eq, sql } from "drizzle-orm";

import { loadConfig } from "../src/config.js";
import { createDb, createPool, runMigrations } from "../src/db/client.js";
import { componentStatus, deviceState, events, telemetry } from "../src/db/schema.js";
import { processTelemetry } from "../src/ingest/process.js";
import { rollupPendingHours } from "../src/jobs/maintenance.js";
import { createLogger } from "../src/logger.js";
import { createRuntime } from "../src/runtime.js";

/**
 * Integração da correção do termômetro (v1.1.2): leitura impossível nunca
 * entra, água fora da faixa habitual vira um aviso por episódio com duração e
 * pico, e o rollup do gráfico só agrega a faixa habitual.
 *
 * Precisa de Postgres de verdade — ver `rollup.test.ts` para como rodar.
 */
const url = process.env["DATABASE_URL"];
const DEVICE = "teste-termometro";
const SEM_HISTORICO = "teste-termometro-novo";
const ROLLUP = "teste-termometro-rollup";

function corpo(deviceId: string, seq: number, celsius: number | null) {
  return {
    device_id: deviceId,
    boot_id: 1,
    seq,
    uptime_ms: 60_000 + seq * 1000,
    config_version: 0,
    light: { on: false, source: "boot" },
    temperature: { celsius, available: true, valid: true, age_ms: 800 },
    fan: { on: false, speed_percent: 0, rpm: 0, mode: "auto" },
    rtc: { available: true, time: "14:30", lost_power: false },
    wifi: { rssi: -46, ip: "192.168.100.46", reconnects: 0 },
  };
}

describe(
  "termômetro: leitura impossível, faixa habitual e rollup (v1.1.2)",
  { skip: url ? false : "DATABASE_URL não definida" },
  () => {
    let rt: ReturnType<typeof createRuntime>;
    // Meio-dia em Recife (15:00Z), para os horários da mensagem serem previsíveis.
    const t0 = new Date("2026-10-02T15:00:00Z");
    const em = (s: number) => new Date(t0.getTime() + s * 1000);
    let seq = 0;

    before(async () => {
      const cfg = loadConfig({
        DATABASE_URL: url,
        DEVICE_INGEST_TOKEN: "token-de-teste-com-tamanho-ok",
        LOG_LEVEL: "fatal",
        NODE_ENV: "test",
        TZ_DISPLAY: "America/Recife",
      });
      const pool = createPool(cfg.DATABASE_URL, 2);
      rt = createRuntime({ cfg, db: createDb(pool), pool, log: createLogger(cfg) });
      await runMigrations(rt.db);
      rt.setDbReady(true);
      await limpar();
    });

    after(async () => {
      await limpar();
      await rt.pool.end();
    });

    async function limpar() {
      for (const d of [DEVICE, SEM_HISTORICO, ROLLUP]) {
        for (const t of ["telemetry", "telemetry_hourly", "events", "commands", "component_status", "settings", "device_state"]) {
          await rt.db.execute(sql`delete from ${sql.identifier(t)} where device_id = ${d}`);
        }
        await rt.db.execute(sql`delete from devices where device_id = ${d}`);
      }
    }

    async function post(deviceId: string, quando: Date, celsius: number | null) {
      const s = sanitizeTelemetryRequest(corpo(deviceId, ++seq, celsius));
      assert.notEqual(s.data, null);
      return processTelemetry(rt.db, rt.cfg, s.data!, quando, s.corrections);
    }

    async function eventos(deviceId: string, code: string) {
      return rt.db
        .select()
        .from(events)
        .where(and(eq(events.deviceId, deviceId), eq(events.code, code)));
    }

    async function estado(deviceId: string) {
      const [s] = await rt.db.select().from(deviceState).where(eq(deviceState.deviceId, deviceId));
      return s;
    }

    it("-48 °C não entra: o estado fica com a última leitura boa e vira um aviso agrupado", async () => {
      await post(DEVICE, em(0), 27.13);
      await post(DEVICE, em(3), -48);
      await post(DEVICE, em(6), -48);

      assert.equal((await estado(DEVICE))?.tempCelsius, 27.13);
      const avisos = await eventos(DEVICE, "temp.implausible");
      assert.equal(avisos.length, 1);
      assert.equal(avisos[0]!.repeatCount, 2);
      assert.match(avisos[0]!.msg, /-48,0 °C/);

      const gravadas = await rt.db
        .select({ c: telemetry.tempCelsius })
        .from(telemetry)
        .where(eq(telemetry.deviceId, DEVICE));
      assert.ok(gravadas.every((r) => r.c !== -48), "o histórico nunca vê o -48");

      const [saude] = await rt.db
        .select()
        .from(componentStatus)
        .where(and(eq(componentStatus.deviceId, DEVICE), eq(componentStatus.comp, "temp")));
      assert.equal(saude?.status, "ok", "nem a saúde");
    });

    it("sem leitura boa anterior, a impossível vira leitura inválida — não 'sensor perdido'", async () => {
      await post(SEM_HISTORICO, em(0), 127.94);
      const s = await estado(SEM_HISTORICO);
      assert.equal(s?.tempCelsius, null);
      assert.equal(s?.tempValid, false);
      const [saude] = await rt.db
        .select()
        .from(componentStatus)
        .where(and(eq(componentStatus.deviceId, SEM_HISTORICO), eq(componentStatus.comp, "temp")));
      assert.equal(saude?.status, "degraded");
    });

    it("água acima de 33 °C: um aviso por episódio, com duração, horário e pico", async () => {
      await post(DEVICE, em(60), 30);
      await post(DEVICE, em(120), 34); // abre — 12:02 em Recife
      await post(DEVICE, em(300), 34.6); // pico novo
      await post(DEVICE, em(400), 34.2);
      await post(DEVICE, em(780), 31); // fecha — 12:13

      const episodios = await eventos(DEVICE, "temp.out_of_usual_range");
      assert.equal(episodios.length, 1);
      const e = episodios[0]!;
      assert.equal(e.sev, "warn");
      assert.equal(e.ctx?.["open"], false);
      assert.equal(e.ctx?.["duration_s"], 660);
      assert.equal(e.ctx?.["peak"], 34.6);
      assert.equal(
        e.msg,
        "Água ficou acima de 33 °C por 11 min, das 12:02 às 12:13 (máx. 34,6 °C)",
      );
    });

    it("abaixo de 16 °C abre outro episódio, no sentido contrário", async () => {
      await post(DEVICE, em(900), 15.5);
      const abertos = (await eventos(DEVICE, "temp.out_of_usual_range")).filter(
        (e) => e.ctx?.["open"] === true,
      );
      assert.equal(abertos.length, 1);
      assert.equal(abertos[0]!.ctx?.["direction"], "below");
      assert.match(abertos[0]!.msg, /abaixo da faixa habitual \(16–33 °C\)/);
      await post(DEVICE, em(960), 20); // fecha, para não vazar para os outros testes
    });

    it("o rollup do gráfico só agrega a faixa habitual", async () => {
      await rt.db.execute(sql`insert into devices (device_id) values (${ROLLUP}) on conflict do nothing`);
      await rt.db.execute(sql`
        insert into telemetry
          (received_at, device_id, boot_id, seq, light_on, temp_celsius, temp_valid,
           fan_on, fan_speed_percent, fan_rpm, fan_mode)
        select date_trunc('hour', now() - interval '3 hours') + (g || ' minutes')::interval,
               ${ROLLUP}, 1, g, false,
               (array[27.0, 34.0, 28.0, 15.0])[1 + (g / 10) % 4], true,
               false, 0, 0, 'auto'
          from generate_series(0, 50, 10) g
      `);
      await rollupPendingHours(rt);

      const r = await rt.db.execute<{ temp_min: number; temp_max: number; samples: number }>(sql`
        select temp_min, temp_max, samples from telemetry_hourly
         where device_id = ${ROLLUP} and hour = date_trunc('hour', now() - interval '3 hours')
      `);
      assert.equal(r.rows[0]?.temp_min, 27);
      assert.equal(r.rows[0]?.temp_max, 28);
      assert.equal(r.rows[0]?.samples, 6, "as amostras fora da faixa contam, só não entram na temperatura");
    });

    it("a 0010 limpa o -48 do histórico e refaz a hora do rollup, e pode rodar de novo", async () => {
      const migration = await readFile(
        new URL("../../drizzle/0010_temperatura_impossivel.sql", import.meta.url),
        "utf8",
      );
      const hora = "date_trunc('hour', now() - interval '2 hours')";
      await rt.db.execute(sql`
        insert into telemetry
          (received_at, device_id, boot_id, seq, light_on, temp_celsius, temp_valid,
           fan_on, fan_speed_percent, fan_rpm, fan_mode)
        select ${sql.raw(hora)} + (g || ' minutes')::interval, ${ROLLUP}, 2, g, false,
               case when g = 30 then -48 else 27.5 end, true, false, 0, 0, 'auto'
          from generate_series(0, 50, 10) g
      `);
      await rt.db.execute(sql`
        insert into telemetry_hourly (device_id, hour, temp_min, temp_avg, temp_max, samples)
        values (${ROLLUP}, ${sql.raw(hora)}, -48, 14.9, 27.5, 6)
        on conflict (device_id, hour) do update
          set temp_min = -48, temp_avg = 14.9, temp_max = 27.5
      `);

      for (const parte of migration.split("--> statement-breakpoint")) {
        await rt.db.execute(sql.raw(parte));
      }
      for (const parte of migration.split("--> statement-breakpoint")) {
        await rt.db.execute(sql.raw(parte)); // idempotente
      }

      const brutos = await rt.db.execute<{ n: string }>(sql`
        select count(*)::text as n from telemetry
         where device_id = ${ROLLUP} and (temp_celsius < 10 or temp_celsius > 45)
      `);
      assert.equal(brutos.rows[0]?.n, "0");
      const h = await rt.db.execute<{ temp_min: number; temp_avg: number; temp_max: number }>(sql`
        select temp_min, temp_avg, temp_max from telemetry_hourly
         where device_id = ${ROLLUP} and hour = ${sql.raw(hora)}
      `);
      assert.equal(h.rows[0]?.temp_min, 27.5);
      assert.equal(h.rows[0]?.temp_avg, 27.5);
      assert.equal(h.rows[0]?.temp_max, 27.5);
    });
  },
);
