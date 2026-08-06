import assert from "node:assert/strict";
import { after, before, describe, it } from "node:test";
import { UINT32_MAX, telemetryRequestSchema } from "@bettacare/contract";
import { eq, sql } from "drizzle-orm";

import { loadConfig } from "../src/config.js";
import { createDb, createPool, runMigrations } from "../src/db/client.js";
import { deviceState, devices } from "../src/db/schema.js";
import { processTelemetry } from "../src/ingest/process.js";
import { createLogger } from "../src/logger.js";
import { createRuntime } from "../src/runtime.js";

/**
 * Regressão do 500 no ingest.
 *
 * O firmware mandava `UINT32_MAX` em `temperature.age_ms` como sentinela de
 * "nunca houve leitura válida". O Zod aceitava (`z.int().min(0)`, sem teto) e a
 * coluna `integer` recusava — exceção dentro da transação, **HTTP 500 em todo
 * POST** enquanto o sensor estivesse sem leitura. O dispositivo lia aquilo como
 * "o servidor quebrou" e reenviava para sempre.
 *
 * Precisa de Postgres de verdade: o que estava errado era a largura da coluna,
 * e nenhum mock reproduz isso. Ver `rollup.test.ts` para como rodar.
 */
const url = process.env["DATABASE_URL"];

const DEVICE = "teste-larguras";

/** Corpo mínimo válido; cada teste sobrescreve só o que lhe interessa. */
function corpo(over: Record<string, unknown> = {}) {
  return telemetryRequestSchema.parse({
    device_id: DEVICE,
    boot_id: 1,
    seq: 1,
    uptime_ms: 60_000,
    config_version: 0,
    light: { on: false, source: "boot" },
    temperature: {
      celsius: null,
      available: false,
      valid: false,
      age_ms: null,
    },
    fan: { on: false, speed_percent: 0, rpm: 0, mode: "auto" },
    rtc: { available: true, time: "14:30", lost_power: false },
    wifi: { rssi: -46, ip: "192.168.100.46", reconnects: 0 },
    ...over,
  });
}

describe(
  "ingest: larguras numéricas",
  { skip: url ? false : "DATABASE_URL não definida" },
  () => {
    let rt: ReturnType<typeof createRuntime>;

    before(async () => {
      const cfg = loadConfig({
        DATABASE_URL: url,
        DEVICE_INGEST_TOKEN: "token-de-teste-com-tamanho-ok",
        LOG_LEVEL: "fatal",
        NODE_ENV: "test",
      });
      const pool = createPool(cfg.DATABASE_URL, 2);
      rt = createRuntime({
        cfg,
        db: createDb(pool),
        pool,
        log: createLogger(cfg),
      });
      await runMigrations(rt.db);
      rt.setDbReady(true);
      await limpar();
    });

    after(async () => {
      await limpar();
      await rt.pool.end();
    });

    async function limpar() {
      // `telemetry` e `events` são particionadas e não têm FK, então saem à mão.
      await rt.db.execute(
        sql`delete from telemetry where device_id = ${DEVICE}`,
      );
      await rt.db.execute(sql`delete from events where device_id = ${DEVICE}`);
      await rt.db.delete(devices).where(eq(devices.deviceId, DEVICE));
    }

    it("aceita a sentinela UINT32_MAX que vinha do firmware antigo", async () => {
      // Firmware 2.0.0 continua no ESP32 e vai mandar isto até ser regravado:
      // o servidor precisa engolir sem quebrar.
      const body = corpo({
        temperature: {
          celsius: null,
          available: false,
          valid: false,
          age_ms: UINT32_MAX,
        },
      });

      const r = await processTelemetry(rt.db, rt.cfg, body, new Date());
      assert.equal(r.response.ok, true);

      const [row] = await rt.db
        .select({ age: deviceState.tempAgeMs })
        .from(deviceState)
        .where(eq(deviceState.deviceId, DEVICE));
      assert.equal(row?.age, UINT32_MAX);
    });

    it("guarda uptime de 25+ dias, que não cabe em integer", async () => {
      // `millis()` passa de INT32_MAX (2.147.483.647) com ~24,8 dias ligado.
      // Sem a coluna em bigint, o aquário derrubaria o próprio ingest ao
      // completar um mês de pé — sozinho, sem ninguém mexer em nada.
      const uptime = 3_000_000_000;
      const body = corpo({ boot_id: 1, seq: 2, uptime_ms: uptime });

      const r = await processTelemetry(rt.db, rt.cfg, body, new Date());
      assert.equal(r.response.ok, true);

      const [row] = await rt.db
        .select({ uptime: deviceState.uptimeMs })
        .from(deviceState)
        .where(eq(deviceState.deviceId, DEVICE));
      assert.equal(row?.uptime, uptime);
    });

    it("registra ausência de leitura como null, não como número grande", async () => {
      const body = corpo({ boot_id: 1, seq: 3 });

      await processTelemetry(rt.db, rt.cfg, body, new Date());

      const [row] = await rt.db
        .select({ age: deviceState.tempAgeMs })
        .from(deviceState)
        .where(eq(deviceState.deviceId, DEVICE));
      assert.equal(row?.age, null);
    });
  },
);
