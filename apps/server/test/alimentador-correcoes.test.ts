import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { after, before, describe, it } from "node:test";
import { sanitizeTelemetryRequest } from "@bettacare/contract";
import { and, eq, sql } from "drizzle-orm";

import { getOverview } from "../src/api/queries.js";
import { loadConfig } from "../src/config.js";
import { createDb, createPool, runMigrations } from "../src/db/client.js";
import { deviceState, events } from "../src/db/schema.js";
import { processTelemetry } from "../src/ingest/process.js";
import { createLogger } from "../src/logger.js";
import { createRuntime } from "../src/runtime.js";

/**
 * Integração da v1.1.1: o bloco do alimentador zerado do firmware 2.0.0 (um
 * `ingest.field_rejected` por POST em produção), o agrupamento de correções
 * repetidas e a limpeza da migration 0009.
 *
 * Precisa de Postgres de verdade — ver `rollup.test.ts` para como rodar.
 */
const url = process.env["DATABASE_URL"];
const DEVICE = "teste-alimentador";
/** O id de produção: a 0009 é restrita a ele, de propósito. */
const PRODUCAO = "aquarium-01";

/** O que o firmware 2.0.0 manda em todo POST quando o módulo nunca respondeu. */
const FEEDER_ZERADO = {
  connected: false,
  auto_enabled: false,
  hour1: 0,
  hour2: 0,
  grains_per_feeding: 0,
  last_feed_age_s: null,
  last_feed_requested: 0,
  last_feed_confirmed: 0,
  last_feed_ok: false,
};

/** Ruído no tacômetro: o exemplo de campo fora de faixa do UPGRADE/05 (C1). */
const RPM_IMPOSSIVEL = { on: true, speed_percent: 50, rpm: 99_999, mode: "auto" };

function corpo(deviceId: string, seq: number, over: Record<string, unknown> = {}) {
  return {
    device_id: deviceId,
    boot_id: 1,
    seq,
    uptime_ms: 60_000 + seq * 1000,
    config_version: 0,
    light: { on: false, source: "boot" },
    temperature: { celsius: 26.0, available: true, valid: true, age_ms: 1000 },
    fan: { on: false, speed_percent: 0, rpm: 0, mode: "auto" },
    rtc: { available: true, time: "14:30", lost_power: false },
    wifi: { rssi: -46, ip: "192.168.100.46", reconnects: 0 },
    ...over,
  };
}

describe(
  "alimentador zerado, correções repetidas e a 0009 (v1.1.1)",
  { skip: url ? false : "DATABASE_URL não definida" },
  () => {
    let rt: ReturnType<typeof createRuntime>;
    const t0 = new Date("2026-10-02T06:00:00Z");
    const em = (s: number) => new Date(t0.getTime() + s * 1000);

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
      await limpar();
    });

    after(async () => {
      await limpar();
      await rt.pool.end();
    });

    async function limpar() {
      for (const d of [DEVICE, PRODUCAO]) {
        for (const t of ["telemetry", "events", "commands", "component_status", "settings", "device_state"]) {
          await rt.db.execute(sql`delete from ${sql.identifier(t)} where device_id = ${d}`);
        }
        await rt.db.execute(sql`delete from devices where device_id = ${d}`);
      }
    }

    async function post(deviceId: string, seq: number, quando: Date, over: Record<string, unknown> = {}) {
      const s = sanitizeTelemetryRequest(corpo(deviceId, seq, over));
      assert.notEqual(s.data, null);
      return processTelemetry(rt.db, rt.cfg, s.data!, quando, s.corrections);
    }

    async function eventos(deviceId: string, code: string) {
      return rt.db
        .select()
        .from(events)
        .where(and(eq(events.deviceId, deviceId), eq(events.code, code)));
    }

    /** Põe as colunas do alimentador exatamente como o firmware 2.0.0 as deixou em produção. */
    async function gravarZeros(deviceId: string) {
      await rt.db
        .update(deviceState)
        .set({
          feederConnected: false,
          feederAutoEnabled: false,
          feederHour1: 0,
          feederHour2: 0,
          feederGrainsPerFeeding: 1,
          feederLastFeedAt: null,
          feederLastFeedRequested: 0,
          feederLastFeedConfirmed: 0,
          feederLastFeedOk: false,
        })
        .where(eq(deviceState.deviceId, deviceId));
    }

    it("módulo que nunca respondeu: nenhum evento, e o app continua vendo 'nenhum módulo'", async () => {
      for (let i = 1; i <= 10; i++) await post(DEVICE, i, em(i), { feeder: FEEDER_ZERADO });

      assert.equal((await eventos(DEVICE, "ingest.field_rejected")).length, 0);
      const o = await getOverview(rt.db, DEVICE);
      assert.equal(o?.state?.feeder, null);
    });

    it("desconectar preserva a última agenda conhecida", async () => {
      await post(DEVICE, 11, em(11), {
        feeder: { connected: true, auto_enabled: true, hour1: 7, hour2: 19, grains_per_feeding: 4 },
      });
      await post(DEVICE, 12, em(12), { feeder: FEEDER_ZERADO });

      const f = (await getOverview(rt.db, DEVICE))?.state?.feeder;
      assert.equal(f?.connected, false);
      assert.equal(f?.hour1, 7);
      assert.equal(f?.hour2, 19);
      assert.equal(f?.grains_per_feeding, 4);
      assert.equal(f?.auto_enabled, true);
    });

    it("refeições em 24 h: gravadas, no overview, e preservadas sem o bloco", async () => {
      await post(DEVICE, 13, em(13), {
        feeder: { connected: true, hour1: 7, hour2: 19, grains_per_feeding: 4, auto_enabled: true, meals_24h: 2 },
      });
      assert.equal((await getOverview(rt.db, DEVICE))?.state?.feeder?.meals_24h, 2);
      await post(DEVICE, 14, em(14)); // módulo fora do fio: sem bloco
      assert.equal((await getOverview(rt.db, DEVICE))?.state?.feeder?.meals_24h, 2, "último conhecido");
    });

    it("limite atingido, reservatório vazio e sensor com defeito tocam o celular", async () => {
      const r = await post(DEVICE, 15, em(15), {
        events: [
          { sev: "warn", comp: "feeder", code: "feeder.limit_reached", msg: "Limite de 3 refeicoes em 24 h: pedido do botao recusado" },
          { sev: "info", comp: "feeder", code: "feeder.fed_ok", msg: "Alimentacao concluida: 5 graos" },
          { sev: "error", comp: "feeder", code: "feeder.hopper_empty", msg: "Nenhum grao caiu em 3 tentativas" },
        ],
      });
      assert.deepEqual(
        r.alerts.map((a) => a.tag),
        ["feeder.limit_reached", "feeder.hopper_empty"],
        "a alimentação normal não notifica",
      );
      assert.match(r.alerts[0]!.title, /limite de refeições/);
      assert.equal(r.alerts[1]!.requireInteraction, true, "o que é erro fica até alguém ver");
    });

    it("a mesma correção em todo POST vira um evento só, com a contagem", async () => {
      const resultados = [];
      for (let i = 0; i < 30; i++) {
        resultados.push(await post(DEVICE, 20 + i, em(20 + i), { fan: RPM_IMPOSSIVEL }));
      }

      const abertos = await eventos(DEVICE, "ingest.field_rejected");
      assert.equal(abertos.length, 1);
      assert.equal(abertos[0]!.repeatCount, 30);
      assert.equal(resultados[0]!.newCorrections.length, 1, "a primeira é notícia");
      assert.ok(
        resultados.slice(1).every((r) => r.newCorrections.length === 0),
        "as demais só somam — e saem do log como debug",
      );
    });

    it("campos diferentes não se somam um no outro", async () => {
      await post(DEVICE, 50, em(50), { wifi: { rssi: 50, ip: "192.168.100.46", reconnects: 0 } });
      const porCampo = (await eventos(DEVICE, "ingest.field_rejected")).map((e) => e.ctx?.["field"]);
      assert.deepEqual(porCampo.sort(), ["fan.rpm", "wifi.rssi"]);
    });

    it("passada a janela de 1 h, a correção abre um evento novo", async () => {
      const r = await post(DEVICE, 60, em(20 + 3600 + 1), { fan: RPM_IMPOSSIVEL });
      assert.equal(r.newCorrections.length, 1);
      const rpm = (await eventos(DEVICE, "ingest.field_rejected")).filter(
        (e) => e.ctx?.["field"] === "fan.rpm",
      );
      assert.equal(rpm.length, 2);
    });

    it("a 0009 limpa só a assinatura dos zeros, só no dispositivo de produção, e pode rodar de novo", async () => {
      const migration = await readFile(
        new URL("../../drizzle/0009_limpa_alimentador_fantasma.sql", import.meta.url),
        "utf8",
      );
      await post(PRODUCAO, 1, em(100));
      await gravarZeros(PRODUCAO);
      await gravarZeros(DEVICE); // mesma assinatura, outro dispositivo: fica como está

      await rt.db.execute(sql.raw(migration));
      await rt.db.execute(sql.raw(migration)); // idempotente

      const [prod] = await rt.db.select().from(deviceState).where(eq(deviceState.deviceId, PRODUCAO));
      assert.equal(prod?.feederConnected, false, "NOT NULL: não pode virar null");
      for (const coluna of [
        "feederAutoEnabled",
        "feederHour1",
        "feederHour2",
        "feederGrainsPerFeeding",
        "feederLastFeedRequested",
        "feederLastFeedConfirmed",
        "feederLastFeedOk",
      ] as const) {
        assert.equal(prod?.[coluna], null, coluna);
      }
      assert.equal((await getOverview(rt.db, PRODUCAO))?.state?.feeder, null, "volta a ser 'nenhum módulo'");

      const [outro] = await rt.db.select().from(deviceState).where(eq(deviceState.deviceId, DEVICE));
      assert.equal(outro?.feederHour1, 0);
      assert.equal(outro?.feederGrainsPerFeeding, 1);

      // Uma agenda de verdade em produção não bate com a assinatura.
      await rt.db
        .update(deviceState)
        .set({ feederAutoEnabled: true, feederHour1: 7, feederHour2: 19, feederGrainsPerFeeding: 1 })
        .where(eq(deviceState.deviceId, PRODUCAO));
      await rt.db.execute(sql.raw(migration));
      const [real] = await rt.db.select().from(deviceState).where(eq(deviceState.deviceId, PRODUCAO));
      assert.equal(real?.feederHour1, 7);
    });
  },
);
