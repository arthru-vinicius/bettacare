import assert from "node:assert/strict";
import { after, before, describe, it } from "node:test";
import { sanitizeTelemetryRequest } from "@bettacare/contract";
import { eq, sql } from "drizzle-orm";

import { loadConfig } from "../src/config.js";
import { createDb, createPool, runMigrations } from "../src/db/client.js";
import { commands, deviceState, events, telemetry } from "../src/db/schema.js";
import { processTelemetry } from "../src/ingest/process.js";
import { tick } from "../src/jobs/watchdog.js";
import { createLogger } from "../src/logger.js";
import { createRuntime } from "../src/runtime.js";

/**
 * Integração da rodada de confiabilidade de 2026-08-21 (UPGRADE/).
 *
 * Precisa de Postgres de verdade — ver `rollup.test.ts` para como rodar.
 */
const url = process.env["DATABASE_URL"];
const DEVICE = "teste-upgrade";

function corpo(over: Record<string, unknown> = {}) {
  return {
    device_id: DEVICE,
    boot_id: 1,
    seq: 1,
    uptime_ms: 60_000,
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
  "tolerância a dado ruim e ciclo de vida do comando (UPGRADE/05 C1, UPGRADE/04 S4)",
  { skip: url ? false : "DATABASE_URL não definida" },
  () => {
    let rt: ReturnType<typeof createRuntime>;

    before(async () => {
      const cfg = loadConfig({
        DATABASE_URL: url,
        DEVICE_INGEST_TOKEN: "token-de-teste-com-tamanho-ok",
        LOG_LEVEL: "fatal",
        NODE_ENV: "test",
        DEVICE_OFFLINE_AFTER_S: "60",
        COMMAND_TTL_S: "90",
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
      await rt.db.execute(sql`delete from telemetry where device_id = ${DEVICE}`);
      await rt.db.execute(sql`delete from events where device_id = ${DEVICE}`);
      await rt.db.execute(sql`delete from commands where device_id = ${DEVICE}`);
      await rt.db.execute(sql`delete from component_status where device_id = ${DEVICE}`);
      await rt.db.execute(sql`delete from device_state where device_id = ${DEVICE}`);
      await rt.db.execute(sql`delete from devices where device_id = ${DEVICE}`);
    }

    it("um rpm impossível é grampeado e aceito, não derruba o POST inteiro", async () => {
      const raw = corpo({ boot_id: 1, seq: 1, fan: { on: true, speed_percent: 80, rpm: 99_999, mode: "auto" } });
      const sanitized = sanitizeTelemetryRequest(raw);
      assert.notEqual(sanitized.data, null);
      assert.equal(sanitized.corrections.length, 1);
      assert.equal(sanitized.corrections[0]?.path, "fan.rpm");

      const r = await processTelemetry(rt.db, rt.cfg, sanitized.data!, new Date(), sanitized.corrections);
      assert.equal(r.response.ok, true);

      const [row] = await rt.db
        .select({ rpm: deviceState.fanRpm })
        .from(deviceState)
        .where(eq(deviceState.deviceId, DEVICE));
      assert.equal(row?.rpm, 20_000);

      const [ev] = await rt.db
        .select()
        .from(events)
        .where(and_code(DEVICE, "ingest.field_rejected"));
      assert.equal(ev?.code, "ingest.field_rejected");
    });

    it("diag chega e é gravado no estado corrente e no histórico", async () => {
      const raw = corpo({
        boot_id: 2,
        seq: 1,
        diag: {
          reset_reason: "POWERON_RESET",
          free_heap: 120_000,
          min_free_heap: 90_000,
          max_alloc_heap: 60_000,
          post_latency_ms: 45,
          api_failures: 0,
          pot_raw_adc: 2048,
          button_pressed: false,
        },
      });
      const sanitized = sanitizeTelemetryRequest(raw);
      assert.notEqual(sanitized.data, null);
      await processTelemetry(rt.db, rt.cfg, sanitized.data!, new Date(), []);

      const [row] = await rt.db
        .select({ resetReason: deviceState.resetReason, minHeap: deviceState.minFreeHeap })
        .from(deviceState)
        .where(eq(deviceState.deviceId, DEVICE));
      assert.equal(row?.resetReason, "POWERON_RESET");
      assert.equal(row?.minHeap, 90_000);
    });

    it("relatório de autodiagnóstico é persistido com o pior status como veredito", async () => {
      const raw = corpo({
        boot_id: 4,
        seq: 1,
        diagnostic: {
          command_id: 77,
          ran_at_uptime_ms: 500_000,
          duration_ms: 340,
          checks: [
            { comp: "temp", status: "ok", detail: "1 sensor, 26.0 C", probed: true },
            { comp: "rtc", status: "ok", detail: "DS3231 em 0x68", probed: true },
            // O pior status manda: um sensor ausente não é compensado por
            // dois componentes saudáveis.
            { comp: "nvs", status: "fault", detail: "Escrita de teste falhou", probed: true },
            { comp: "light", status: "unknown", detail: "SSR sem retorno", probed: false },
          ],
        },
      });
      const sanitized = sanitizeTelemetryRequest(raw);
      assert.notEqual(sanitized.data, null, "o relatório precisa passar no contrato");
      await processTelemetry(rt.db, rt.cfg, sanitized.data!, new Date(), []);

      const [row] = await rt.db
        .select({
          overall: deviceState.diagnosticOverall,
          duration: deviceState.diagnosticDurationMs,
          cmd: deviceState.diagnosticCommandId,
          checks: deviceState.diagnosticChecks,
        })
        .from(deviceState)
        .where(eq(deviceState.deviceId, DEVICE));

      assert.equal(row?.overall, "fault");
      assert.equal(row?.duration, 340);
      assert.equal(row?.cmd, 77);
      assert.equal(row?.checks?.length, 4);

      // E o desfecho vira evento, para aparecer na aba Logs junto com o resto
      // da história — não só numa tela que alguém precisa lembrar de abrir.
      const [ev] = await rt.db
        .select()
        .from(events)
        .where(and_code(DEVICE, "system.diagnostic_found_problem"));
      assert.equal(ev?.sev, "warn");
    });

    it("um POST comum não apaga o relatório anterior", async () => {
      const sanitized = sanitizeTelemetryRequest(corpo({ boot_id: 4, seq: 2 }));
      await processTelemetry(rt.db, rt.cfg, sanitized.data!, new Date(), []);

      const [row] = await rt.db
        .select({ overall: deviceState.diagnosticOverall })
        .from(deviceState)
        .where(eq(deviceState.deviceId, DEVICE));
      assert.equal(row?.overall, "fault", "o relatório precisa sobreviver aos ciclos normais");
    });

    it("'unknown' nao conta como problema — a luminaria nunca e verificavel", async () => {
      // Se contasse, todo diagnostico terminaria em alerta por causa do SSR
      // sem retorno, e um alarme que sempre dispara se aprende a ignorar.
      const raw = corpo({
        boot_id: 5,
        seq: 1,
        diagnostic: {
          ran_at_uptime_ms: 1000,
          duration_ms: 100,
          checks: [
            { comp: "temp", status: "ok", detail: "1 sensor", probed: true },
            { comp: "light", status: "unknown", detail: "SSR sem retorno", probed: false },
          ],
        },
      });
      const sanitized = sanitizeTelemetryRequest(raw);
      await processTelemetry(rt.db, rt.cfg, sanitized.data!, new Date(), []);

      const ok = await rt.db
        .select()
        .from(events)
        .where(and_code(DEVICE, "system.diagnostic_ran"));
      assert.equal(ok.length, 1, "deve registrar como execucao limpa");

      // O veredito ainda reflete o `unknown`, que e mais grave que `ok` —
      // so nao e tratado como defeito encontrado.
      const [row] = await rt.db
        .select({ overall: deviceState.diagnosticOverall })
        .from(deviceState)
        .where(eq(deviceState.deviceId, DEVICE));
      assert.equal(row?.overall, "unknown");
    });

    it("comando ainda 'queued' com o dispositivo mudo vira cmd.device_offline", async () => {
      // Faz o dispositivo existir (via um POST normal) e depois o silencia.
      const sanitized = sanitizeTelemetryRequest(corpo({ boot_id: 3, seq: 1 }));
      await processTelemetry(rt.db, rt.cfg, sanitized.data!, new Date(), []);

      const [cmd] = await rt.db
        .insert(commands)
        .values({ deviceId: DEVICE, target: "light", action: "light.set", payload: { on: true }, status: "queued" })
        .returning({ id: commands.id });

      // Empurra `last_seen_at` para 20 s atrás — acima do QUEUED_OFFLINE_AFTER_S de 15 s.
      await rt.db.execute(
        sql`update devices set last_seen_at = now() - interval '20 seconds' where device_id = ${DEVICE}`,
      );

      await tick(rt);

      const [row] = await rt.db
        .select({ errorCode: commands.errorCode, status: commands.status })
        .from(commands)
        .where(eq(commands.id, cmd!.id));
      assert.equal(row?.status, "queued", "continua esperando — não é 'expired'");
      assert.equal(row?.errorCode, "cmd.device_offline");

      const [ev] = await rt.db
        .select()
        .from(events)
        .where(and_code(DEVICE, "cmd.device_offline"));
      assert.equal(ev?.code, "cmd.device_offline");

      // Um segundo tick não deve duplicar o evento.
      await tick(rt);
      const contagem = await rt.db
        .select({ n: sql<string>`count(*)` })
        .from(events)
        .where(and_code(DEVICE, "cmd.device_offline"));
      assert.equal(contagem[0]?.n, "1");
    });
  },
);

function and_code(deviceId: string, code: string) {
  return sql`device_id = ${deviceId} and code = ${code}`;
}
