import assert from "node:assert/strict";
import { describe, it } from "node:test";
import type { TemperaturePoint } from "@bettacare/contract";

import { eixoY, localizar, marcasX, resumir, segmentar } from "../lib/grafico.ts";

/**
 * A matemática do gráfico de temperatura: eixos, buracos e o ponto sob o dedo.
 * Roda em qualquer fuso — as asserções de horário usam o relógio local do
 * próprio processo, que é exatamente o que o navegador faz.
 */

const ponto = (t: number, v = 27): TemperaturePoint => [t, v, v, v];

describe("eixo Y", () => {
  it("usa números redondos, com folga, e entre 3 e 7 marcas", () => {
    for (const [min, max] of [
      [27.0, 28.0],
      [19.06, 28.75],
      [26.9, 27.1],
      [21.3, 21.8],
      [27.3, 27.3],
      [10, 45],
    ] as const) {
      const e = eixoY(min, max);
      assert.ok(e.lo <= min && e.hi >= max, `${min}–${max} cabe em ${e.lo}–${e.hi}`);
      assert.ok(e.ticks.length >= 3 && e.ticks.length <= 8, `${e.ticks.length} marcas para ${min}–${max}`);
      assert.equal(e.ticks[0], e.lo);
      assert.equal(e.ticks.at(-1), e.hi);
      const passo = e.ticks[1]! - e.ticks[0]!;
      assert.ok([0.5, 1, 2, 5, 10].includes(Math.round(passo * 100) / 100), `passo ${passo}`);
    }
  });

  it("uma água estável abre marcas dos dois lados em vez de colar a linha numa borda", () => {
    const e = eixoY(27.3, 27.3);
    assert.ok(e.lo < 27.3 && e.hi > 27.3);
  });
});

describe("eixo X, no relógio local", () => {
  const agora = new Date(2026, 9, 22, 14, 30).getTime() / 1000;

  it("24 h: de 4 em 4 horas, em hora cheia", () => {
    const marcas = marcasX("24h", agora - 24 * 3600, agora);
    assert.ok(marcas.length >= 5 && marcas.length <= 7, `${marcas.length} marcas`);
    for (const m of marcas) {
      const d = new Date(m.t * 1000);
      assert.equal(d.getHours() % 4, 0);
      assert.equal(d.getMinutes(), 0);
      assert.equal(m.label, `${String(d.getHours()).padStart(2, "0")}h`);
      assert.ok(m.t >= agora - 24 * 3600 && m.t <= agora);
    }
    for (let i = 1; i < marcas.length; i++) {
      assert.equal((marcas[i]!.t - marcas[i - 1]!.t) / 3600, 4);
    }
  });

  it("7 dias: um rótulo por dia, centrado nele; os cortados pelas bordas ficam estreitos", () => {
    const marcas = marcasX("7d", agora - 7 * 86400, agora);
    assert.deepEqual(
      marcas.map((m) => m.label),
      ["qui 15", "sex 16", "sáb 17", "dom 18", "seg 19", "ter 20", "qua 21", "qui 22"],
    );
    const dia = 86400;
    // Os dias inteiros têm 24 h; o primeiro (a partir das 14:30) e o último (até as 14:30) são parciais.
    assert.equal(marcas[0]!.span, 9.5 * 3600);
    assert.equal(marcas.at(-1)!.span, 14.5 * 3600);
    for (const m of marcas.slice(1, -1)) assert.equal(m.span, dia);
    // Só os começos que são meia-noite de verdade levam linha de grade.
    assert.deepEqual(
      marcas.map((m) => m.limite),
      [false, true, true, true, true, true, true, true],
    );
    for (const m of marcas) assert.ok(m.centro >= m.t && m.centro <= agora);
    for (const m of marcas.filter((x) => x.limite)) assert.equal(new Date(m.t * 1000).getHours(), 0);
  });

  it("30 dias: de 5 em 5 dias contando de hoje para trás", () => {
    const marcas = marcasX("30d", agora - 30 * 86400, agora);
    assert.deepEqual(
      marcas.map((m) => m.label),
      ["27/09", "02/10", "07/10", "12/10", "17/10", "22/10"],
    );
    assert.ok(marcas.every((m) => m.limite && m.centro === m.t));
  });
});

describe("buracos na série", () => {
  it("parte a linha onde o aquário ficou mudo", () => {
    const pontos = [0, 60, 120, 600, 660].map((t) => ponto(t));
    const segmentos = segmentar(pontos, 150);
    assert.deepEqual(
      segmentos.map((s) => s.map((p) => p[0])),
      [
        [0, 60, 120],
        [600, 660],
      ],
    );
  });

  it("um minuto sem leitura (heartbeat atrasado) não parte a linha de 24 h", () => {
    const pontos = [0, 60, 180, 240].map((t) => ponto(t));
    assert.equal(segmentar(pontos, 150).length, 1);
  });

  it("série vazia dá nenhum segmento", () => {
    assert.deepEqual(segmentar([], 150), []);
  });
});

describe("o que está sob o dedo", () => {
  const pontos = [0, 60, 120, 600, 660].map((t) => ponto(t));
  const f = (t: number, fim = 1000) => localizar(pontos, t, 60, 150, fim);

  it("dentro do intervalo de um ponto é sempre esse ponto", () => {
    assert.deepEqual(f(0), { tipo: "ponto", i: 0 });
    assert.deepEqual(f(59.9), { tipo: "ponto", i: 0 });
    assert.deepEqual(f(125), { tipo: "ponto", i: 2 });
    assert.deepEqual(f(610), { tipo: "ponto", i: 3 });
  });

  it("num silêncio mostra o silêncio, não o ponto vizinho", () => {
    assert.deepEqual(f(200), { tipo: "buraco", de: 180, ate: 600 });
    assert.deepEqual(f(590), { tipo: "buraco", de: 180, ate: 600 });
  });

  it("entre dois pontos de um trecho contínuo, o mais perto", () => {
    const dois = [ponto(0), ponto(120)];
    assert.deepEqual(localizar(dois, 70, 60, 150, 1000), { tipo: "ponto", i: 0 }, "10 s do fim do primeiro");
    assert.deepEqual(localizar(dois, 100, 60, 150, 1000), { tipo: "ponto", i: 1 }, "20 s do início do segundo");
  });

  it("depois do último ponto: contínuo se a série é recente, silêncio se ficou velha", () => {
    const recente = [ponto(900)];
    assert.deepEqual(localizar(recente, 980, 60, 150, 1000), { tipo: "ponto", i: 0 });
    assert.deepEqual(f(800, 1000), { tipo: "buraco", de: 720, ate: 1000 });
  });

  it("antes do primeiro ponto é silêncio; sem pontos, vazio", () => {
    const r = f(-100);
    assert.equal(r.tipo, "buraco");
    assert.equal(r.tipo === "buraco" && r.ate, 0);
    assert.deepEqual(localizar([], 10, 60, 150, 1000), { tipo: "vazio" });
  });
});

describe("resumo para a tabela", () => {
  it("24 h: um grupo por hora cheia local, com mínima, média e máxima", () => {
    const h = (hora: number, min: number) => new Date(2026, 9, 22, hora, min).getTime() / 1000;
    const pontos: TemperaturePoint[] = [
      [h(13, 5), 27.0, 26.9, 27.1],
      [h(13, 50), 27.4, 27.3, 27.6],
      [h(14, 10), 28.0, 27.9, 28.2],
    ];
    const linhas = resumir(pontos, "24h");
    assert.equal(linhas.length, 2);
    assert.equal(linhas[0]!.t, h(13, 0));
    assert.deepEqual([linhas[0]!.min, linhas[0]!.avg, linhas[0]!.max], [26.9, 27.2, 27.6]);
    assert.deepEqual([linhas[1]!.min, linhas[1]!.avg, linhas[1]!.max], [27.9, 28, 28.2]);
  });

  it("7 e 30 dias: um grupo por dia local", () => {
    const d = (dia: number, hora: number) => new Date(2026, 9, dia, hora).getTime() / 1000;
    const pontos: TemperaturePoint[] = [
      [d(20, 23), 25, 24.5, 25.5],
      [d(21, 1), 24, 23.5, 24.5],
      [d(21, 15), 28, 27.5, 28.5],
    ];
    const linhas = resumir(pontos, "7d");
    assert.deepEqual(
      linhas.map((l) => l.t),
      [d(20, 0), d(21, 0)],
    );
    assert.equal(linhas[1]!.max, 28.5);
    assert.equal(linhas[1]!.min, 23.5);
  });
});
