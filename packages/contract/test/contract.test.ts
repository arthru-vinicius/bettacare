import assert from "node:assert/strict";
import { describe, it } from "node:test";

import {
  DEFAULT_CONFIG,
  describeEventCode,
  deviceConfigSchema,
  pendingCommandSchema,
  summarizeHealth,
  targetOf,
  telemetryRequestSchema,
  telemetryResponseSchema,
  type ComponentHealth,
} from "../dist/index.js";

/** POST representativo, com todos os blocos preenchidos. */
const post = {
  device_id: "aquarium-01",
  boot_id: 42,
  seq: 1337,
  uptime_ms: 84_213_000,
  device_time: "2026-08-04T17:31:02Z",
  config_version: 7,
  light: { on: true, source: "schedule" },
  temperature: { celsius: 26.5, available: true, valid: true, age_ms: 1200 },
  fan: { on: false, speed_percent: 0, rpm: 0, mode: "auto" },
  rtc: { available: true, time: "14:32" },
  wifi: { rssi: -58, ip: "192.168.0.42" },
  ack: [{ id: 91, ok: false, code: "light.gpio_fault" }],
  events: [
    {
      sev: "error",
      comp: "temp",
      code: "temp.sensor_lost",
      msg: "DS18B20 nao responde",
    },
  ],
};

describe("telemetria: requisição", () => {
  it("aceita o POST completo e aplica os defaults", () => {
    const r = telemetryRequestSchema.parse(post);
    assert.equal(r.events[0]?.repeat_count, 1);
    assert.equal(r.rtc.lost_power, false);
    assert.equal(r.wifi.reconnects, 0);
  });

  it("aceita o POST mínimo — firmware sem nada a reportar", () => {
    const { ack: _a, events: _e, ...minimo } = post;
    const r = telemetryRequestSchema.parse(minimo);
    assert.deepEqual(r.ack, []);
    assert.deepEqual(r.events, []);
  });

  it("representa sensor ausente com null, não com sentinela", () => {
    const r = telemetryRequestSchema.parse({
      ...post,
      temperature: {
        celsius: null,
        available: false,
        valid: false,
        age_ms: 99_999,
      },
    });
    assert.equal(r.temperature.celsius, null);
  });

  it("aceita age_ms nulo — nunca houve leitura válida", () => {
    const r = telemetryRequestSchema.parse({
      ...post,
      temperature: {
        celsius: null,
        available: false,
        valid: false,
        age_ms: null,
      },
    });
    assert.equal(r.temperature.age_ms, null);
  });

  it("limita todo inteiro ao que a coluna aguenta", () => {
    // Sem teto, um valor que o Zod aceita e o PostgreSQL recusa vira exceção
    // no meio da transação e responde 500 — o dispositivo entende "o servidor
    // quebrou" e reenvia para sempre. Com teto, a recusa é um 400 honesto.
    const acimaDoUint32 = { ...post, uptime_ms: 4_294_967_296 };
    assert.equal(telemetryRequestSchema.safeParse(acimaDoUint32).success, false);

    const seqAcimaDoInt32 = { ...post, seq: 2_147_483_648 };
    assert.equal(
      telemetryRequestSchema.safeParse(seqAcimaDoInt32).success,
      false,
    );

    // O uptime real de 25 dias precisa continuar passando: é uint32 legítimo,
    // não um erro.
    assert.equal(
      telemetryRequestSchema.safeParse({ ...post, uptime_ms: 3_000_000_000 })
        .success,
      true,
    );
  });

  it("recusa device_id que não seja slug", () => {
    assert.equal(
      telemetryRequestSchema.safeParse({ ...post, device_id: "Aquario 01" })
        .success,
      false,
    );
  });
});

describe("eventos", () => {
  it("recusa código sem o formato componente.motivo", () => {
    assert.equal(
      telemetryRequestSchema.safeParse({
        ...post,
        events: [{ sev: "info", comp: "temp", code: "SEM_PONTO", msg: "x" }],
      }).success,
      false,
    );
  });

  it("aceita código desconhecido — firmware novo não derruba o ingest", () => {
    const code = "temp.motivo_que_ainda_nao_existe";
    const r = telemetryRequestSchema.parse({
      ...post,
      events: [{ sev: "info", comp: "temp", code, msg: "x" }],
    });
    assert.equal(r.events[0]?.code, code);
    assert.equal(describeEventCode(code), undefined);
  });

  it("traduz os códigos conhecidos, com dica onde há ação a tomar", () => {
    const info = describeEventCode("light.gpio_fault");
    assert.equal(info?.sev, "error");
    assert.ok(info?.hint);
  });
});

describe("comandos", () => {
  it("preserva id e payload do comando pendente", () => {
    const c = pendingCommandSchema.parse({
      id: 91,
      action: "light.set",
      on: true,
    });
    assert.equal(c.id, 91);
    assert.equal(c.action, "light.set");
    assert.equal(c.on, true);
  });

  it("não existe light.toggle — comandos são estado desejado", () => {
    assert.equal(
      pendingCommandSchema.safeParse({ id: 1, action: "light.toggle" }).success,
      false,
    );
  });

  it("mapeia cada ação ao alvo que ela supersede", () => {
    assert.equal(targetOf({ action: "light.set", on: true }), "light");
    assert.equal(targetOf({ action: "fan.set_speed", percent: 50 }), "fan");
    assert.equal(targetOf({ action: "fan.set_mode", mode: "auto" }), "fan");
    assert.equal(targetOf({ action: "device.reboot" }), "device");
  });
});

describe("telemetria: resposta", () => {
  const base = {
    ok: true as const,
    server_time: "2026-08-04T17:31:03Z",
    config_version: 8,
  };

  it("cabe folgadamente no orçamento de 300 bytes do contrato", () => {
    const comComando = telemetryResponseSchema.parse({
      ...base,
      commands: [{ id: 91, action: "light.set", on: true }],
    });
    assert.ok(Buffer.byteLength(JSON.stringify(comComando)) < 300);
  });

  it("omite config quando o dispositivo já está na versão corrente", () => {
    const normal = telemetryResponseSchema.parse(base);
    assert.equal(normal.config, undefined);
    assert.ok(Buffer.byteLength(JSON.stringify(normal)) < 120);
  });
});

describe("configuração", () => {
  it("recusa histerese invertida", () => {
    assert.equal(
      deviceConfigSchema.safeParse({
        ...DEFAULT_CONFIG,
        fan_trigger_c: 27.0,
        fan_off_c: 29.0,
      }).success,
      false,
    );
  });

  it("o padrão é válido", () => {
    assert.equal(deviceConfigSchema.safeParse(DEFAULT_CONFIG).success, true);
  });
});

describe("saúde agregada", () => {
  const ok: ComponentHealth = {
    comp: "temp",
    status: "ok",
    since: "2026-08-04T00:00:00Z",
    last_ok_at: null,
    last_code: null,
    detail: null,
  };

  it("offline vence tudo — dado velho não vira 'tudo ok'", () => {
    assert.equal(summarizeHealth([ok], false), "offline");
    assert.equal(summarizeHealth([{ ...ok, status: "fault" }], false), "offline");
  });

  it("classifica falha e instabilidade", () => {
    assert.equal(summarizeHealth([ok], true), "ok");
    assert.equal(summarizeHealth([{ ...ok, status: "missing" }], true), "critical");
    assert.equal(summarizeHealth([{ ...ok, status: "fault" }], true), "critical");
    assert.equal(
      summarizeHealth([{ ...ok, status: "degraded" }], true),
      "attention",
    );
  });
});
