import assert from "node:assert/strict";
import { after, before, beforeEach, describe, it } from "node:test";
import { sanitizeTelemetryRequest } from "@bettacare/contract";
import { and, eq, sql } from "drizzle-orm";

import { enqueueCommand, listCommands } from "../src/api/queries.js";
import { loadConfig } from "../src/config.js";
import { createDb, createPool, runMigrations } from "../src/db/client.js";
import { commands, devices, events } from "../src/db/schema.js";
import { processTelemetry } from "../src/ingest/process.js";
import { tick } from "../src/jobs/watchdog.js";
import { createLogger } from "../src/logger.js";
import { createRuntime } from "../src/runtime.js";

/**
 * Tolerância ao Wi-Fi ruim do aquário (v1.3.0, 2026-10-14).
 *
 * Em produção o controlador ficou 103 vezes sem contato numa semana — mediana
 * de 2,3 min, p90 de 13 min. Antes, um comando entregue numa resposta que se
 * perdia morria 90 s depois, e cada queda tocava o celular. Estes testes
 * fixam o comportamento novo: o comando espera o prazo de vida inteiro, o
 * reinício não repete o que não pode ser repetido, e o push só sai para
 * silêncio longo.
 *
 * Precisa de Postgres de verdade — ver `rollup.test.ts` para como rodar.
 */
const url = process.env["DATABASE_URL"];
const DEVICE = "teste-rede";
const MAX_AGE_S = 900;

function corpo(over: Record<string, unknown> = {}) {
  return {
    device_id: DEVICE,
    boot_id: 10,
    seq: 1,
    uptime_ms: 60_000,
    config_version: 0,
    light: { on: false, source: "boot" },
    temperature: { celsius: 26.0, available: true, valid: true, age_ms: 1000 },
    fan: { on: false, speed_percent: 0, rpm: 0, mode: "auto" },
    rtc: { available: true, time: "14:30", lost_power: false },
    wifi: { rssi: -66, ip: "192.168.100.46", reconnects: 0 },
    ...over,
  };
}

describe(
  "rede instável: fila de comandos, reinício e alerta de offline",
  { skip: url ? false : "DATABASE_URL não definida" },
  () => {
    let rt: ReturnType<typeof createRuntime>;
    let seq = 1;

    before(async () => {
      const cfg = loadConfig({
        DATABASE_URL: url,
        DEVICE_INGEST_TOKEN: "token-de-teste-com-tamanho-ok",
        LOG_LEVEL: "fatal",
        NODE_ENV: "test",
        COMMAND_MAX_AGE_S: String(MAX_AGE_S),
        DEVICE_OFFLINE_AFTER_S: "60",
        DEVICE_OFFLINE_ALERT_AFTER_S: "900",
        // A variável obsoleta continua aceita — o compose de produção a declara.
        COMMAND_TTL_S: "90",
      });
      const pool = createPool(cfg.DATABASE_URL, 2);
      rt = createRuntime({ cfg, db: createDb(pool), pool, log: createLogger(cfg) });
      await runMigrations(rt.db);
      rt.setDbReady(true);
    });

    beforeEach(async () => {
      await limpar();
      seq = 1;
      await post();
    });

    after(async () => {
      await limpar();
      await rt.pool.end();
    });

    async function limpar() {
      for (const t of ["telemetry", "events", "commands", "component_status", "settings", "device_state"]) {
        await rt.db.execute(sql`delete from ${sql.identifier(t)} where device_id = ${DEVICE}`);
      }
      await rt.db.execute(sql`delete from devices where device_id = ${DEVICE}`);
    }

    async function post(over: Record<string, unknown> = {}, quando = new Date()) {
      const s = sanitizeTelemetryRequest(corpo({ seq: seq++, ...over }));
      assert.notEqual(s.data, null);
      return processTelemetry(rt.db, rt.cfg, s.data!, quando, s.corrections);
    }

    async function comando(id: number) {
      const [row] = await rt.db.select().from(commands).where(eq(commands.id, id));
      return row!;
    }

    async function eventos(code: string) {
      return rt.db
        .select()
        .from(events)
        .where(and(eq(events.deviceId, DEVICE), eq(events.code, code)));
    }

    /** Recua os relógios do comando, como se ele tivesse sido criado/entregue no passado. */
    async function envelhecer(id: number, criadoHaS: number, entregueHaS: number | null) {
      await rt.db.execute(sql`
        update commands
           set created_at = now() - make_interval(secs => ${criadoHaS}),
               sent_at = ${entregueHaS === null ? null : sql`now() - make_interval(secs => ${entregueHaS})`}
         where id = ${id}
      `);
    }

    async function silenciar(haS: number) {
      await rt.db.execute(
        sql`update devices set last_seen_at = now() - make_interval(secs => ${haS}) where device_id = ${DEVICE}`,
      );
    }

    it("comando entregue sobrevive a uma queda de 5 min e é reentregue na volta", async () => {
      const id = await enqueueCommand(rt.db, DEVICE, { action: "light.set", on: true }, null);
      const r1 = await post();
      assert.deepEqual(r1.response.commands.map((c) => c.id), [id], "entregue na primeira resposta");

      // A resposta se perdeu e o aquário sumiu por 5 min — antes, aos 90 s,
      // este comando já estaria morto.
      await envelhecer(id, 300, 300);
      await silenciar(300);
      await tick(rt);
      assert.equal((await comando(id)).status, "sent", "continua valendo");

      const r2 = await post();
      assert.deepEqual(r2.response.commands.map((c) => c.id), [id], "reentregue na volta");

      await post({ ack: [{ id, ok: true }] });
      assert.equal((await comando(id)).status, "acked");
    });

    it("entregue e sem confirmação por todo o prazo vira cmd.expired", async () => {
      const id = await enqueueCommand(rt.db, DEVICE, { action: "light.set", on: true }, null);
      await post();
      await envelhecer(id, MAX_AGE_S + 60, MAX_AGE_S + 60);
      await tick(rt);

      const c = await comando(id);
      assert.equal(c.status, "expired");
      assert.equal(c.errorCode, "cmd.expired");
      assert.equal((await eventos("cmd.expired")).length, 1);
    });

    it("entregue no último minuto ganha folga para o ack voltar", async () => {
      const id = await enqueueCommand(rt.db, DEVICE, { action: "light.set", on: true }, null);
      await post();
      // Criado há 15 min e 20 s, mas entregue há só 30 s: sem a folga, venceria
      // antes de a confirmação ter tempo de chegar.
      await envelhecer(id, MAX_AGE_S + 20, 30);
      await tick(rt);
      assert.equal((await comando(id)).status, "sent");

      const [item] = await listCommands(rt.db, DEVICE, 5, MAX_AGE_S);
      assert.ok(item?.expires_at, "a lista diz até quando ele vale");
      // 30 s depois da entrega, restam ~90 s da folga de 2 min. A margem é do
      // relógio do Postgres, que pode estar uns décimos à frente do Node.
      const restante = Date.parse(item!.expires_at!) - Date.now();
      assert.ok(restante > 80_000 && restante < 100_000, `restam ${restante} ms`);
    });

    it("nunca entregue dentro do prazo vira cmd.stale_discarded, sem executar", async () => {
      await silenciar(MAX_AGE_S + 120);
      const id = await enqueueCommand(rt.db, DEVICE, { action: "fan.set_speed", percent: 40 }, null);
      await envelhecer(id, MAX_AGE_S + 60, null);
      await tick(rt);

      const c = await comando(id);
      assert.equal(c.status, "expired");
      assert.equal(c.errorCode, "cmd.stale_discarded");

      // E não sai na próxima resposta, mesmo que o aquário volte agora.
      const r = await post();
      assert.equal(r.response.commands.length, 0);
    });

    it("reinício: reboot confirmado, alimentação encerrada, luz reentregue", async () => {
      const reboot = await enqueueCommand(rt.db, DEVICE, { action: "device.reboot" }, null);
      const luz = await enqueueCommand(rt.db, DEVICE, { action: "light.set", on: true }, null);
      const racao = await enqueueCommand(rt.db, DEVICE, { action: "feeder.feed_now", grains: 3 }, null);
      const r1 = await post();
      assert.equal(r1.response.commands.length, 3, "os três foram numa resposta");

      // Boot novo antes de qualquer ack chegar.
      seq = 0;
      const r2 = await post({ boot_id: 11 });

      assert.equal((await comando(reboot)).status, "acked", "o reinício pedido aconteceu");
      const feed = await comando(racao);
      assert.equal(feed.status, "expired");
      assert.equal(feed.errorCode, "cmd.interrupted");
      assert.equal((await eventos("cmd.interrupted")).length, 1);

      assert.deepEqual(
        r2.response.commands.map((c) => c.id),
        [luz],
        "a luz é estado desejado: reentregar é inofensivo",
      );
    });

    it("POST atrasado do boot anterior não encerra nada", async () => {
      const racao = await enqueueCommand(rt.db, DEVICE, { action: "feeder.feed_now" }, null);
      seq = 0;
      await post({ boot_id: 11 }); // boot novo entrega a alimentação
      assert.equal((await comando(racao)).status, "sent");

      // Um corpo do boot 10, com seq alto, chegando depois.
      seq = 500;
      await post({ boot_id: 10 });
      assert.equal((await comando(racao)).status, "sent", "continua esperando o ack do boot 11");
    });

    it("offline: evento em 60 s, push só depois de 15 min, e a volta avisa", async () => {
      await silenciar(120);
      await tick(rt);
      let [d] = await rt.db.select().from(devices).where(eq(devices.deviceId, DEVICE));
      assert.equal(d?.online, false);
      assert.equal(d?.offlineAlertedAt, null, "queda curta não toca o celular");
      assert.equal((await eventos("device.offline")).length, 1);

      // 16 min e meio: longe da fronteira do minuto, que o relógio do
      // Postgres (uns décimos à frente do Node) faria oscilar.
      await silenciar(16 * 60 + 30);
      await tick(rt);
      [d] = await rt.db.select().from(devices).where(eq(devices.deviceId, DEVICE));
      assert.notEqual(d?.offlineAlertedAt, null, "silêncio longo notifica, uma vez");

      const volta = await post();
      [d] = await rt.db.select().from(devices).where(eq(devices.deviceId, DEVICE));
      assert.equal(d?.online, true);
      assert.equal(d?.offlineAlertedAt, null, "episódio encerrado");

      const [online] = await eventos("device.online");
      assert.match(online!.msg, /depois de 16 min sem contato/);
      assert.ok(
        volta.alerts.some((a) => a.title === "Controlador voltou" && a.tag === "device-offline"),
        "a volta substitui o alarme no celular",
      );
    });

    it("volta de queda curta não manda push nenhum", async () => {
      await silenciar(120);
      await tick(rt);
      const volta = await post();
      assert.equal(volta.alerts.some((a) => a.tag === "device-offline"), false);
    });
  },
);
