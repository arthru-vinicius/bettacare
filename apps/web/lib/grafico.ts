import type { FanRun, HistoryRange, TemperaturePoint } from "@bettacare/contract";

/**
 * Matemática do gráfico de temperatura: escalas, eixos, buracos e o ponto sob o
 * dedo. Funções puras, sem DOM e sem React — a parte que erra em silêncio
 * (um eixo sem sentido, um ponto escolhido errado, uma linha ligando um
 * silêncio) fica testável em `test/grafico.test.ts`.
 *
 * Tempo em **segundos Unix**, como a API entrega. Os eixos usam o relógio local
 * do navegador: é ele que diz quando é meia-noite para quem está olhando.
 */

// ── Eixo da temperatura ───────────────────────────────────────────────────

export interface EixoY {
  lo: number;
  hi: number;
  ticks: number[];
}

const PASSOS_Y = [0.5, 1, 2, 5, 10];

/**
 * Domínio e marcas do eixo Y para `[min, max]`: números redondos (27, 27,5),
 * folga em cima e embaixo para a linha não encostar na borda, e entre 3 e 7
 * marcas — mais que isso é grade, não eixo.
 */
export function eixoY(min: number, max: number): EixoY {
  const span = Math.max(max - min, 0.5);
  const folga = span * 0.08;
  let passo = PASSOS_Y.at(-1)!;
  for (const p of PASSOS_Y) {
    const lo = Math.floor((min - folga) / p) * p;
    const hi = Math.ceil((max + folga) / p) * p;
    if ((hi - lo) / p <= 6) {
      passo = p;
      break;
    }
  }
  let lo = Math.floor((min - folga) / passo) * passo;
  let hi = Math.ceil((max + folga) / passo) * passo;
  // Uma água estável cabe numa faixa só: abre uma marca para cada lado, senão o
  // eixo teria duas e a linha ficaria colada numa borda.
  if ((hi - lo) / passo < 2) {
    lo -= passo;
    hi += passo;
  }
  const ticks: number[] = [];
  for (let v = lo; v <= hi + passo / 1000; v += passo) ticks.push(Math.round(v * 100) / 100);
  return { lo, hi, ticks };
}

// ── Eixo do tempo ─────────────────────────────────────────────────────────

export interface MarcaX {
  /** Instante da marca (segundos). Em 7 dias, o começo do dia dentro da janela. */
  t: number;
  label: string;
  /** Onde o rótulo se centra: a própria marca, ou — em 7 dias — o meio do dia. */
  centro: number;
  /** Largura visível do que a marca nomeia, em segundos: um dia cortado pela borda é estreito demais para ter rótulo. */
  span: number;
  /** A marca é uma fronteira de verdade (meia-noite dentro da janela): leva uma linha de grade. */
  limite: boolean;
}

const DIAS = ["dom", "seg", "ter", "qua", "qui", "sex", "sáb"];
const dois = (n: number) => String(n).padStart(2, "0");

/**
 * Marcas do eixo X no relógio local: de 4 em 4 horas em 24 h ("08h", "12h"), um
 * rótulo por dia em 7 dias ("qua 22", centrado no dia, como em qualquer
 * calendário), e de 5 em 5 dias em 30 dias ("17/10").
 */
export function marcasX(range: HistoryRange, t0: number, t1: number): MarcaX[] {
  const marcas: MarcaX[] = [];

  if (range === "24h") {
    // A primeira hora cheia, múltipla de 4, a partir do começo da janela.
    const d = new Date(t0 * 1000);
    d.setMinutes(0, 0, 0);
    if (d.getTime() / 1000 < t0) d.setHours(d.getHours() + 1);
    while (d.getHours() % 4 !== 0) d.setHours(d.getHours() + 1);
    for (let guarda = 0; d.getTime() / 1000 <= t1 && guarda < 20; guarda++) {
      const t = d.getTime() / 1000;
      marcas.push({ t, label: `${dois(d.getHours())}h`, centro: t, span: Infinity, limite: false });
      d.setHours(d.getHours() + 4);
    }
    return marcas;
  }

  if (range === "7d") {
    // Um rótulo por dia, no meio dele; os dias cortados pelas bordas ficam
    // estreitos e o componente decide se cabe rótulo.
    const d = new Date(t0 * 1000);
    d.setHours(0, 0, 0, 0);
    for (let guarda = 0; guarda < 10; guarda++) {
      const ini = d.getTime() / 1000;
      const prox = new Date(d);
      prox.setDate(prox.getDate() + 1);
      const fim = prox.getTime() / 1000;
      const a = Math.max(ini, t0);
      const b = Math.min(fim, t1);
      if (a < t1) {
        marcas.push({
          t: a,
          label: `${DIAS[d.getDay()]} ${d.getDate()}`,
          centro: (a + b) / 2,
          span: b - a,
          limite: ini >= t0,
        });
      }
      if (fim >= t1) break;
      d.setTime(fim * 1000);
    }
    return marcas;
  }

  const passoDias = 5;
  // Meia-noite de hoje, contando para trás: a marca mais à direita é sempre a de hoje.
  const hoje = new Date(t1 * 1000);
  hoje.setHours(0, 0, 0, 0);
  for (let n = 0; n < 40; n++) {
    const d = new Date(hoje);
    d.setDate(hoje.getDate() - n * passoDias);
    const t = d.getTime() / 1000;
    if (t < t0) break;
    if (t <= t1) {
      marcas.push({
        t,
        label: `${dois(d.getDate())}/${dois(d.getMonth() + 1)}`,
        centro: t,
        span: Infinity,
        limite: true,
      });
    }
  }
  return marcas.reverse();
}

// ── Buracos ───────────────────────────────────────────────────────────────

/**
 * Parte a série onde o aquário ficou mudo. Ligar o último ponto antes do
 * silêncio ao primeiro depois desenharia uma reta — uma temperatura que ninguém
 * mediu.
 */
export function segmentar(points: TemperaturePoint[], gapS: number): TemperaturePoint[][] {
  const segmentos: TemperaturePoint[][] = [];
  let atual: TemperaturePoint[] = [];
  for (const p of points) {
    const anterior = atual.at(-1);
    if (anterior !== undefined && p[0] - anterior[0] > gapS) {
      segmentos.push(atual);
      atual = [];
    }
    atual.push(p);
  }
  if (atual.length > 0) segmentos.push(atual);
  return segmentos;
}

/**
 * Os intervalos em que não houve leitura: entre trechos, antes do primeiro
 * ponto e depois do último (aquário sem contato até agora). Em segundos.
 */
export function buracosDaSerie(
  points: TemperaturePoint[],
  bucketS: number,
  gapS: number,
  t0: number,
  t1: number,
): Array<[number, number]> {
  const buracos: Array<[number, number]> = [];
  const segmentos = segmentar(points, gapS);
  const primeiro = segmentos[0]?.[0]?.[0];
  if (primeiro !== undefined && primeiro - t0 > gapS) buracos.push([t0, primeiro]);
  for (let i = 0; i + 1 < segmentos.length; i++) {
    buracos.push([segmentos[i]!.at(-1)![0] + bucketS, segmentos[i + 1]![0]![0]]);
  }
  const ultimo = segmentos.at(-1)?.at(-1)?.[0];
  if (ultimo !== undefined && t1 - ultimo > gapS) buracos.push([ultimo + bucketS, t1]);
  return buracos;
}

/**
 * As linhas "liga/desliga" da ventoinha só entram no gráfico se a água está
 * perto delas. Com a água a 21 °C e a ventoinha ligando a 29, esticar o eixo
 * até lá achata a linha e esconde justamente a variação que se quer ler.
 */
export function limitesRelevantes(
  min: number,
  max: number,
  limites: { liga: number; desliga: number } | null,
): boolean {
  return limites !== null && limites.liga >= min - 3 && limites.desliga <= max + 3;
}

export type Alvo =
  | { tipo: "ponto"; i: number }
  /** O instante cai num silêncio: `de`–`ate` (segundos) é o que não foi medido. */
  | { tipo: "buraco"; de: number; ate: number }
  | { tipo: "vazio" };

/**
 * O que está sob o instante `t`: o ponto mais próximo, ou o silêncio em que ele
 * cai.
 *
 * Dentro de um trecho contínuo (buraco menor que `gapS`) o ponto é o mais
 * próximo — a linha passa ali, então há um valor para mostrar. Num silêncio
 * (a linha está interrompida), mostrar o ponto vizinho seria afirmar uma
 * leitura que não houve: aí o alvo é o próprio buraco. Um instante ainda dentro
 * do intervalo de um ponto é sempre esse ponto.
 *
 * `fim` é o fim do eixo (agora): um último ponto recente ainda "continua" até lá.
 */
export function localizar(
  points: TemperaturePoint[],
  t: number,
  bucketS: number,
  gapS: number,
  fim: number,
): Alvo {
  if (points.length === 0) return { tipo: "vazio" };

  // Primeiro ponto que começa depois de `t`.
  let a = 0;
  let b = points.length;
  while (a < b) {
    const m = (a + b) >> 1;
    if (points[m]![0] > t) b = m;
    else a = m + 1;
  }
  const antes = a > 0 ? a - 1 : -1;
  const depois = a < points.length ? a : -1;

  if (antes >= 0 && t < points[antes]![0] + bucketS) return { tipo: "ponto", i: antes };

  const fimAntes = antes >= 0 ? points[antes]![0] + bucketS : -Infinity;
  const inicioDepois = depois >= 0 ? points[depois]![0] : Infinity;
  const proximo = depois >= 0 ? inicioDepois : fim;
  const continuo = antes < 0 ? false : proximo - points[antes]![0] <= gapS;

  if (continuo && antes >= 0) {
    if (depois < 0) return { tipo: "ponto", i: antes };
    // Entre dois pontos de um trecho contínuo: o mais perto.
    return t - fimAntes <= inicioDepois - t ? { tipo: "ponto", i: antes } : { tipo: "ponto", i: depois };
  }
  return { tipo: "buraco", de: antes >= 0 ? fimAntes : -Infinity, ate: depois >= 0 ? inicioDepois : fim };
}

/** A rodada da ventoinha que cobre o instante `t`, se houver (rodada aberta vai até `fim`). */
export function rodadaEm(runs: FanRun[], t: number, fim: number): FanRun | undefined {
  return runs.find((r) => {
    const ini = Date.parse(r.started_at) / 1000;
    const f = r.ended_at ? Date.parse(r.ended_at) / 1000 : fim;
    return t >= ini && t <= f;
  });
}

// ── Tabela-irmã do gráfico ────────────────────────────────────────────────

export interface LinhaResumo {
  /** Início do período (segundos), no relógio local. */
  t: number;
  min: number;
  avg: number;
  max: number;
}

/**
 * Resume a série por hora (24 h) ou por dia (7 e 30 dias), no relógio local —
 * a versão em tabela do gráfico, para quem não usa ou não vê o gráfico.
 */
export function resumir(points: TemperaturePoint[], range: HistoryRange): LinhaResumo[] {
  const grupos = new Map<number, { soma: number; n: number; min: number; max: number }>();
  for (const [t, avg, min, max] of points) {
    const d = new Date(t * 1000);
    if (range === "24h") d.setMinutes(0, 0, 0);
    else d.setHours(0, 0, 0, 0);
    const k = d.getTime() / 1000;
    const g = grupos.get(k);
    if (g === undefined) grupos.set(k, { soma: avg, n: 1, min, max });
    else {
      g.soma += avg;
      g.n += 1;
      g.min = Math.min(g.min, min);
      g.max = Math.max(g.max, max);
    }
  }
  return [...grupos.entries()]
    .sort((x, y) => x[0] - y[0])
    .map(([t, g]) => ({
      t,
      min: g.min,
      avg: Math.round((g.soma / g.n) * 100) / 100,
      max: g.max,
    }));
}
