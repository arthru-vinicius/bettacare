import assert from "node:assert/strict";
import { after, before, describe, it } from "node:test";
import { DEFAULT_CONFIG, sanitizeTelemetryRequest } from "@bettacare/contract";
import { and, eq, sql } from "drizzle-orm";

import { eventosCsv, medicoesCsv, parsePeriodo } from "../src/api/export.js";
import { enqueueCommand, getOverview, updateSettings } from "../src/api/queries.js";
import { loadConfig } from "../src/config.js";
import { createDb, createPool, runMigrations } from "../src/db/client.js";
import { componentStatus, events, settings } from "../src/db/schema.js";
import { processTelemetry } from "../src/ingest/process.js";
import { createLogger } from "../src/logger.js";
import { createRuntime } from "../src/runtime.js";

/**
 * Integração da rodada de 2026-10-02 (UPGRADE/07): configuração editável pela
 * interface, falso alarme da luminária, escrita de saúde sob POST a cada 1 s e
 * exportação para planilha.
 *
 * Precisa de Postgres de verdade — ver `rollup.test.ts` para como rodar.
 */
const url = process.env["DATABASE_URL"];
const DEVICE = "teste-rodada-ux";

function corpo(seq: number, over: Record<string, unknown> = {}) {
  return sanitizeTelemetryRequest({
    device_id: DEVICE,
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
  });
}

/**
 * Lê o fluxo inteiro. Devolve o texto **com** o BOM: o `Response.text()` o
 * descartaria ao decodificar, e é justamente ele que o teste quer ver.
 */
async function lerFluxo(s: ReadableStream<Uint8Array>): Promise<string> {
  const bytes = new Uint8Array(await new Response(s).arrayBuffer());
  return new TextDecoder("utf-8", { ignoreBOM: true }).decode(bytes);
}

describe("rodada de UX e estabilidade (UPGRADE/07)", { skip: url ? false : "DATABASE_URL não definida" }, () => {
  let rt: ReturnType<typeof createRuntime>;
  const t0 = new Date("2026-10-02T15:00:00Z");
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
    for (const t of ["telemetry", "events", "commands", "component_status", "settings", "device_state"]) {
      await rt.db.execute(sql`delete from ${sql.identifier(t)} where device_id = ${DEVICE}`);
    }
    await rt.db.execute(sql`delete from devices where device_id = ${DEVICE}`);
  }

  async function post(seq: number, quando: Date, over: Record<string, unknown> = {}) {
    const s = corpo(seq, over);
    assert.notEqual(s.data, null);
    return processTelemetry(rt.db, rt.cfg, s.data!, quando, s.corrections);
  }

  it("configuração: grava, registra quem mudou o quê, e não sobe a versão se nada mudou", async () => {
    await post(1, em(0)); // o dispositivo precisa existir antes da configuração

    const nova = { ...DEFAULT_CONFIG, light_on_time: "09:00", light_off_time: "18:30" };
    const r1 = await updateSettings(rt.db, DEVICE, nova, "arthur@exemplo");
    assert.equal(r1.ok, true);
    if (!r1.ok) return;

    const r2 = await updateSettings(rt.db, DEVICE, nova, "arthur@exemplo");
    assert.equal(r2.ok && r2.version, r1.version, "a mesma configuração não pode subir a versão");
    assert.equal(r2.ok && r2.changed, false);

    const registros = await rt.db
      .select()
      .from(events)
      .where(and(eq(events.deviceId, DEVICE), eq(events.code, "settings.updated")));
    assert.equal(registros.length, 1);
    assert.match(registros[0]!.msg, /acende 09:00 e apaga 18:30/);
    assert.match(registros[0]!.msg, /arthur@exemplo/);
  });

  it("configuração inválida é recusada — horários iguais deixariam a luz acesa para sempre", async () => {
    const r = await updateSettings(
      rt.db,
      DEVICE,
      { ...DEFAULT_CONFIG, light_on_time: "08:00", light_off_time: "08:00" },
      null,
    );
    assert.equal(r.ok, false);
  });

  it("o overview diz qual versão da configuração o aquário já aplicou", async () => {
    const [cfg] = await rt.db.select().from(settings).where(eq(settings.deviceId, DEVICE));
    await post(2, em(1), { config_version: cfg!.configVersion });
    const o = await getOverview(rt.db, DEVICE);
    assert.equal(o?.state?.config_version, cfg!.configVersion);
    assert.equal(o?.config_version, cfg!.configVersion);
  });

  it("luz trocada pelo botão depois de um comando do app não vira falha da luminária", async () => {
    const id = await enqueueCommand(rt.db, DEVICE, { action: "light.set", on: true }, null);
    await post(3, em(2)); // entrega o comando
    await post(4, em(3), {
      light: { on: true, source: "command" },
      ack: [{ id, ok: true }],
    });
    // O usuário aperta o botão físico e apaga: dez segundos de POSTs a cada 1 s.
    for (let i = 0; i < 10; i++) {
      await post(5 + i, em(4 + i), { light: { on: false, source: "button" } });
    }

    const [luz] = await rt.db
      .select()
      .from(componentStatus)
      .where(and(eq(componentStatus.deviceId, DEVICE), eq(componentStatus.comp, "light")));
    assert.equal(luz?.status, "ok");

    const falsos = await rt.db
      .select()
      .from(events)
      .where(and(eq(events.deviceId, DEVICE), eq(events.code, "light.state_mismatch")));
    assert.equal(falsos.length, 0);
  });

  it("com POST a cada 1 s, a saúde não é regravada a cada POST", async () => {
    const antes = await rt.db
      .select({ updatedAt: componentStatus.updatedAt })
      .from(componentStatus)
      .where(and(eq(componentStatus.deviceId, DEVICE), eq(componentStatus.comp, "temp")));
    // Mais dois POSTs dentro da janela de 5 s desde o último que gravou.
    const ultimo = antes[0]!.updatedAt.getTime();
    await post(20, new Date(ultimo + 1000));
    await post(21, new Date(ultimo + 2000));
    const depois = await rt.db
      .select({ updatedAt: componentStatus.updatedAt })
      .from(componentStatus)
      .where(and(eq(componentStatus.deviceId, DEVICE), eq(componentStatus.comp, "temp")));
    assert.equal(depois[0]!.updatedAt.getTime(), ultimo, "nada mudou: não precisava escrever");

    // Uma mudança de status, essa sim, sai na hora.
    await post(22, new Date(ultimo + 3000), {
      temperature: { celsius: null, available: false, valid: false, age_ms: null },
    });
    const [temp] = await rt.db
      .select()
      .from(componentStatus)
      .where(and(eq(componentStatus.deviceId, DEVICE), eq(componentStatus.comp, "temp")));
    assert.equal(temp?.status, "missing");
  });

  it("evento de componente desconhecido é descartado sozinho e vira aviso, o resto entra", async () => {
    const s = sanitizeTelemetryRequest({
      ...corpo(30).data,
      events: [
        { sev: "info", comp: "light", code: "light.on", msg: "acesa" },
        { sev: "info", comp: "periferico_novo", code: "periferico_novo.x", msg: "?" },
      ],
    });
    assert.notEqual(s.data, null);
    await processTelemetry(rt.db, rt.cfg, s.data!, em(40), s.corrections);

    const descartes = await rt.db
      .select()
      .from(events)
      .where(and(eq(events.deviceId, DEVICE), eq(events.code, "ingest.event_dropped")));
    assert.equal(descartes.length, 1);
    assert.match(String(descartes[0]!.ctx?.["received"]), /periferico_novo/);
  });

  it("planilha de registros: só o período e os filtros pedidos, no formato do Excel em português", async () => {
    const periodo = parsePeriodo(em(-60).toISOString(), em(3600).toISOString());
    assert.notEqual(periodo, null);
    const csv = await lerFluxo(
      eventosCsv(rt.db, { deviceId: DEVICE, sevs: ["info"], ...periodo! }, "America/Recife"),
    );
    assert.ok(csv.startsWith("﻿"), "BOM para o Excel ler os acentos");
    // `trim()` também come o BOM (U+FEFF conta como espaço para ele).
    const linhas = csv.trim().split("\r\n");
    assert.match(linhas[0]!, /^Data e hora;Nível;Origem;Equipamento;Evento;Mensagem/);
    assert.ok(linhas.length > 1);
    for (const l of linhas.slice(1)) assert.match(l, /;Informação;/);
    // Fuso de exibição: 15:00Z é meio-dia em Recife.
    assert.ok(linhas.some((l) => l.startsWith("02/10/2026 12:00")));
  });

  it("planilha de medições: vírgula decimal e uma linha por amostra gravada", async () => {
    const periodo = parsePeriodo(em(-60).toISOString(), em(3600).toISOString())!;
    const csv = await lerFluxo(medicoesCsv(rt.db, { deviceId: DEVICE, ...periodo }, "America/Recife"));
    const linhas = csv.trim().split("\r\n");
    assert.match(linhas[0]!, /Temperatura \(°C\)/);
    assert.ok(linhas.slice(1).some((l) => l.includes(";26,00;")));
  });

  it("evento e comandos do alimentador cabem nos enums do banco (migration 0008)", async () => {
    // O contrato já tinha "feeder" e as duas ações; o banco não — o primeiro
    // evento do módulo, ou o primeiro toque em "Alimentar agora", seria 500.
    await post(40, em(60), {
      events: [
        { sev: "info", comp: "feeder", code: "feeder.module_connected", msg: "Modulo respondeu" },
      ],
    });
    const [ev] = await rt.db
      .select()
      .from(events)
      .where(and(eq(events.deviceId, DEVICE), eq(events.code, "feeder.module_connected")));
    assert.equal(ev?.comp, "feeder");

    await enqueueCommand(rt.db, DEVICE, { action: "feeder.feed_now" }, null);
    await enqueueCommand(
      rt.db,
      DEVICE,
      { action: "feeder.set_config", hour1: 7, hour2: 19, grains_per_feeding: 4, auto_enabled: true },
      null,
    );
  });

  it("período inválido, invertido ou longo demais é recusado", () => {
    assert.equal(parsePeriodo("ontem", undefined), null);
    assert.equal(parsePeriodo(em(10).toISOString(), em(0).toISOString()), null);
    assert.equal(parsePeriodo("2024-01-01T00:00:00Z", "2026-01-01T00:00:00Z"), null);
  });
});
