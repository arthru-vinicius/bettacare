import assert from "node:assert/strict";
import { describe, it } from "node:test";
import { telemetryRequestSchema, type TelemetryRequest } from "@bettacare/contract";

import { evaluateHealth, type HealthContext } from "../src/health/evaluate.js";

/**
 * Testes puros — sem banco — das regras acrescentadas na rodada de
 * confiabilidade de 2026-08-21 (UPGRADE/04, S2 e S3). `evaluateHealth` não
 * toca infraestrutura nenhuma, então roda em qualquer máquina sem Docker.
 */

const T0 = new Date("2026-10-02T12:00:00Z");
const CTX_VAZIO: HealthContext = { previousDetail: {}, lightDesired: null, now: T0 };

/**
 * Roda uma sequência de POSTs espaçados por `passoMs`, encadeando o `detail`
 * de cada avaliação na seguinte — como o ingest faz com o que gravou no banco.
 */
function sequencia(
  comp: "fan" | "light" | "button",
  bodies: TelemetryRequest[],
  passoMs: number,
  lightDesired: boolean | null = null,
) {
  let ctx: HealthContext = CTX_VAZIO;
  let ultimo;
  bodies.forEach((b, i) => {
    ctx = { ...ctx, lightDesired, now: new Date(T0.getTime() + i * passoMs) };
    ultimo = evaluateHealth(b, ctx).find((v) => v.comp === comp);
    ctx = { ...ctx, previousDetail: { [comp]: ultimo?.detail ?? null } };
  });
  return ultimo as ReturnType<typeof evaluateHealth>[number] | undefined;
}

/**
 * `over` é tipado como entrada crua, não como `Partial<TelemetryRequest>`:
 * `TelemetryRequest` é o tipo de **saída** do Zod, onde todo campo com
 * `.default()` já é obrigatório. Um teste que só quer sobrescrever
 * `pot_raw_adc` não deveria ter de repetir os cinco defaults do bloco.
 */
function corpo(over: Record<string, unknown> = {}): TelemetryRequest {
  return telemetryRequestSchema.parse({
    device_id: "aquarium-01",
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
  });
}

describe("saúde: button e pot (UPGRADE/04, S2)", () => {
  it("sem diag, button e pot não aparecem — nunca 'unknown' fingido", () => {
    const verdicts = evaluateHealth(corpo(), CTX_VAZIO);
    assert.equal(verdicts.find((v) => v.comp === "button"), undefined);
    assert.equal(verdicts.find((v) => v.comp === "pot"), undefined);
  });

  it("com diag, button e pot entram como ok por padrão", () => {
    const verdicts = evaluateHealth(
      corpo({ diag: { button_pressed: false, pot_raw_adc: 2000, api_failures: 0 } }),
      CTX_VAZIO,
    );
    const button = verdicts.find((v) => v.comp === "button");
    const pot = verdicts.find((v) => v.comp === "pot");
    assert.equal(button?.status, "ok");
    assert.equal(pot?.status, "ok");
  });

  it("potenciômetro saturado no topo do curso vira degraded", () => {
    const verdicts = evaluateHealth(
      corpo({ diag: { pot_raw_adc: 4095, button_pressed: false, api_failures: 0 } }),
      CTX_VAZIO,
    );
    const pot = verdicts.find((v) => v.comp === "pot");
    assert.equal(pot?.status, "degraded");
    assert.equal(pot?.code, "pot.out_of_range");
  });

  it("botão pressionado por mais de 30 s vira degraded (preso)", () => {
    const pressionado = corpo({ diag: { button_pressed: true, api_failures: 0 } });
    const ultimo = sequencia("button", Array(32).fill(pressionado), 1000);
    assert.equal(ultimo?.status, "degraded");
    assert.equal(ultimo?.code, "button.stuck");
  });

  it("botão segurado 10 s com POST a cada 1 s não é preso — a regra é por tempo, não por POST", () => {
    // Com a regra antiga (10 strikes), isto já virava "preso" a 1 s por POST.
    const pressionado = corpo({ diag: { button_pressed: true, api_failures: 0 } });
    assert.equal(sequencia("button", Array(11).fill(pressionado), 1000)?.status, "ok");
  });

  it("botão pressionado uma vez só não dispara nada — evita alarme por toque normal", () => {
    const verdicts = evaluateHealth(
      corpo({ diag: { button_pressed: true, api_failures: 0 } }),
      CTX_VAZIO,
    );
    assert.equal(verdicts.find((v) => v.comp === "button")?.status, "ok");
  });
});

describe("saúde: anomalias medidas por tempo (UPGRADE/07)", () => {
  const girando = corpo({ fan: { on: true, speed_percent: 60, rpm: 1400, mode: "manual" } });
  const parada = corpo({ fan: { on: true, speed_percent: 60, rpm: 0, mode: "manual" } });

  it("ventoinha recém-ligada com tacômetro ainda em zero não é 'travada'", () => {
    // O firmware conta pulsos em janelas de 2 s: logo depois de ligar, alguns
    // POSTs chegam com rpm 0. Com POST a cada 1 s, a regra antiga de dois
    // strikes acusava falha (e mandava push) em toda partida.
    const v = sequencia("fan", [parada, parada, parada, girando], 1000);
    assert.equal(v?.status, "ok");
    assert.equal(v?.detail?.["anomaly_since"], null);
  });

  it("ventoinha parada por 5 s com PWM vira fault, qualquer que seja o intervalo", () => {
    assert.equal(sequencia("fan", Array(6).fill(parada), 1000)?.status, "fault");
    assert.equal(sequencia("fan", Array(3).fill(parada), 3000)?.status, "fault");
    assert.equal(sequencia("fan", Array(5).fill(parada), 1000)?.status, "ok");
  });

  it("luz trocada pelo botão físico depois de um comando não é falha da luminária", () => {
    // O caso de produção: 300 `light.state_mismatch` falsos num dia — o
    // "desejado" do último comando contra o estado que o botão mudou.
    const peloBotao = corpo({ light: { on: false, source: "button" } });
    const v = sequencia("light", Array(10).fill(peloBotao), 1000, true);
    assert.equal(v?.status, "ok");
  });

  it("estado diferente do comandado com a origem ainda no comando, por 5 s, é falha", () => {
    const divergente = corpo({ light: { on: false, source: "command" } });
    assert.equal(sequencia("light", Array(6).fill(divergente), 1000, true)?.status, "fault");
    assert.equal(sequencia("light", Array(2).fill(divergente), 1000, true)?.status, "ok");
  });
});

describe("saúde: api real (UPGRADE/04, S3)", () => {
  it("sem falhas anteriores, ok", () => {
    const verdicts = evaluateHealth(corpo({ diag: { api_failures: 0, button_pressed: false } }), CTX_VAZIO);
    assert.equal(verdicts.find((v) => v.comp === "api")?.status, "ok");
  });

  it("com falhas anteriores registradas, degraded — mesmo este POST tendo chegado", () => {
    const verdicts = evaluateHealth(corpo({ diag: { api_failures: 3, button_pressed: false } }), CTX_VAZIO);
    const api = verdicts.find((v) => v.comp === "api");
    assert.equal(api?.status, "degraded");
    assert.equal(api?.code, "api.post_failed");
  });

  it("sem bloco diag (firmware antigo), continua a tautologia — não regride", () => {
    const verdicts = evaluateHealth(corpo(), CTX_VAZIO);
    assert.equal(verdicts.find((v) => v.comp === "api")?.status, "ok");
  });
});

describe("saúde: nvs, ota e system — os 3 que faltavam", () => {
  it("sem diag, os três não aparecem", () => {
    const verdicts = evaluateHealth(corpo(), CTX_VAZIO);
    for (const comp of ["nvs", "ota", "system"]) {
      assert.equal(verdicts.find((v) => v.comp === comp), undefined, comp);
    }
  });

  it("nvs saudável quando não houve falha de escrita", () => {
    const verdicts = evaluateHealth(corpo({ diag: { nvs_failures: 0 } }), CTX_VAZIO);
    assert.equal(verdicts.find((v) => v.comp === "nvs")?.status, "ok");
  });

  it("qualquer falha de NVS já é degradação — a config não sobrevive ao reboot", () => {
    const verdicts = evaluateHealth(corpo({ diag: { nvs_failures: 1 } }), CTX_VAZIO);
    const nvs = verdicts.find((v) => v.comp === "nvs");
    assert.equal(nvs?.status, "degraded");
    assert.equal(nvs?.code, "nvs.write_failed");
  });

  it("OTA que falhou vira fault — antes era completamente invisível", () => {
    const verdicts = evaluateHealth(
      corpo({ diag: { ota_last_result: "failed" } }),
      CTX_VAZIO,
    );
    const ota = verdicts.find((v) => v.comp === "ota");
    assert.equal(ota?.status, "fault");
    assert.equal(ota?.code, "ota.failed");
  });

  it("nunca ter atualizado é tão saudável quanto ter atualizado com sucesso", () => {
    for (const resultado of ["none", "ok"]) {
      const verdicts = evaluateHealth(
        corpo({ diag: { ota_last_result: resultado } }),
        CTX_VAZIO,
      );
      assert.equal(verdicts.find((v) => v.comp === "ota")?.status, "ok", resultado);
    }
  });

  it("reinício por brownout aponta hardware e vira fault", () => {
    const verdicts = evaluateHealth(
      corpo({ diag: { reset_reason: "brownout", free_heap: 200_000 } }),
      CTX_VAZIO,
    );
    const sys = verdicts.find((v) => v.comp === "system");
    assert.equal(sys?.status, "fault");
    assert.equal(sys?.code, "system.unclean_reset");
  });

  it("ligar na tomada e reiniciar por comando são saudáveis, não falha", () => {
    for (const motivo of ["poweron", "sw"]) {
      const verdicts = evaluateHealth(
        corpo({ diag: { reset_reason: motivo, free_heap: 200_000 } }),
        CTX_VAZIO,
      );
      assert.equal(verdicts.find((v) => v.comp === "system")?.status, "ok", motivo);
    }
  });

  it("heap baixo vira degraded", () => {
    const verdicts = evaluateHealth(
      corpo({ diag: { reset_reason: "poweron", free_heap: 20_000 } }),
      CTX_VAZIO,
    );
    const sys = verdicts.find((v) => v.comp === "system");
    assert.equal(sys?.status, "degraded");
    assert.equal(sys?.code, "system.low_memory");
  });

  it("evento perdido por estouro é degradação da própria capacidade de investigar", () => {
    const verdicts = evaluateHealth(
      corpo({ diag: { reset_reason: "poweron", free_heap: 200_000, events_dropped: 3 } }),
      CTX_VAZIO,
    );
    const sys = verdicts.find((v) => v.comp === "system");
    assert.equal(sys?.status, "degraded");
    assert.equal(sys?.code, "system.event_overflow");
  });
});
