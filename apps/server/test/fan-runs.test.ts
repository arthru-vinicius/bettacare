import assert from "node:assert/strict";
import { describe, it } from "node:test";

import {
  planFanRun,
  RUN_GAP_MS,
  type FanObservation,
  type FanPrevious,
} from "../src/ingest/fan-runs.js";

/**
 * A decisão de abrir, fechar ou continuar uma rodada da ventoinha. Pura — roda
 * sem banco. A execução no Postgres é coberta em `acionamentos.test.ts`.
 */

const T0 = new Date("2026-10-22T12:00:00Z");
const em = (s: number) => new Date(T0.getTime() + s * 1000);

const obs = (over: Partial<FanObservation> = {}): FanObservation => ({
  on: true,
  mode: "auto",
  speedPercent: 30,
  celsius: 29.1,
  bootId: 5,
  uptimeMs: 3_600_000,
  at: em(10),
  ...over,
});
const prev = (over: Partial<FanPrevious> = {}): FanPrevious => ({
  on: false,
  at: em(7),
  bootId: 5,
  ...over,
});

describe("rodadas da ventoinha: o que cada POST significa", () => {
  it("desligada antes e agora não faz nada", () => {
    assert.deepEqual(planFanRun(prev(), obs({ on: false })), { kind: "none" });
  });

  it("ligou entre dois POSTs vizinhos: abre, com o horário exato do POST", () => {
    const o = obs({ at: em(10) });
    assert.deepEqual(planFanRun(prev({ at: em(7) }), o), {
      kind: "open",
      startedAt: o.at,
      approx: false,
    });
  });

  it("ligou depois de um silêncio longo: abre, mas o início é aproximado", () => {
    const o = obs({ at: em(600) });
    const step = planFanRun(prev({ at: em(0) }), o);
    assert.equal(step.kind, "open");
    assert.equal(step.kind === "open" && step.approx, true);
  });

  it("primeiro contato já com a ventoinha ligada: abre aproximada", () => {
    const step = planFanRun(null, obs());
    assert.equal(step.kind, "open");
    assert.equal(step.kind === "open" && step.approx, true);
  });

  it("desligou entre POSTs vizinhos: fecha agora, com a temperatura de saída", () => {
    assert.deepEqual(planFanRun(prev({ on: true, at: em(7) }), obs({ on: false })), {
      kind: "close",
      useLastOn: false,
    });
  });

  it("desligou depois de um silêncio longo: o fim é a última vez que se viu ligada", () => {
    // Pode ter desligado em qualquer ponto do silêncio; afirmar "agora" seria
    // inventar duração.
    const step = planFanRun(
      prev({ on: true, at: em(0) }),
      obs({ on: false, at: em(RUN_GAP_MS / 1000 + 5) }),
    );
    assert.deepEqual(step, { kind: "close", useLastOn: true });
  });

  it("o silêncio de exatamente 5 min ainda é exato; um segundo a mais já não", () => {
    const limite = planFanRun(
      prev({ on: true, at: em(0) }),
      obs({ on: false, at: em(RUN_GAP_MS / 1000) }),
    );
    assert.deepEqual(limite, { kind: "close", useLastOn: false });
    const passou = planFanRun(
      prev({ on: true, at: em(0) }),
      obs({ on: false, at: em(RUN_GAP_MS / 1000 + 1) }),
    );
    assert.deepEqual(passou, { kind: "close", useLastOn: true });
  });

  it("segue ligada no mesmo boot: continua", () => {
    assert.deepEqual(planFanRun(prev({ on: true }), obs()), { kind: "continue", approx: false });
  });

  it("segue ligada depois de um silêncio longo: continua, marcada como aproximada", () => {
    assert.deepEqual(planFanRun(prev({ on: true, at: em(0) }), obs({ at: em(900) })), {
      kind: "continue",
      approx: true,
    });
  });

  it("reiniciou ligada: a rodada velha acabou no reinício e outra começa", () => {
    // O PWM zera no reset. O reinício foi `uptime` atrás, entre os dois POSTs.
    const o = obs({ bootId: 6, at: em(40), uptimeMs: 25_000 });
    const step = planFanRun(prev({ on: true, at: em(7), bootId: 5 }), o);
    assert.equal(step.kind, "reopen");
    if (step.kind !== "reopen") return;
    assert.equal(step.closeAt?.getTime(), em(15).getTime(), "40 s - 25 s de uptime");
    assert.equal(step.startedAt.getTime(), o.at.getTime());
  });

  it("o reinício nunca é datado antes do último POST que viu a ventoinha ligada", () => {
    const o = obs({ bootId: 6, at: em(40), uptimeMs: 1_000_000 });
    const step = planFanRun(prev({ on: true, at: em(7), bootId: 5 }), o);
    assert.equal(step.kind === "reopen" && step.closeAt?.getTime(), em(7).getTime());
  });

  it("reiniciou ligada depois de silêncio longo: o fim da velha é o último que se viu", () => {
    const o = obs({ bootId: 6, at: em(3600), uptimeMs: 20_000 });
    const step = planFanRun(prev({ on: true, at: em(0), bootId: 5 }), o);
    assert.deepEqual(step, { kind: "reopen", closeAt: null, startedAt: o.at });
  });

  it("reiniciou e a ventoinha ficou desligada: é um desligamento comum", () => {
    const step = planFanRun(
      prev({ on: true, at: em(7), bootId: 5 }),
      obs({ on: false, bootId: 6 }),
    );
    assert.equal(step.kind, "close");
  });
});
