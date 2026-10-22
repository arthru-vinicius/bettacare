"use client";

import type { FanHistoryResponse, FanRun, TemperaturePoint } from "@bettacare/contract";
import {
  useEffect,
  useMemo,
  useRef,
  useState,
  type KeyboardEvent,
  type PointerEvent,
  type RefObject,
} from "react";

import { formatDia, formatHora, formatTemp, secondsAgo } from "@/lib/format";
import {
  buracosDaSerie,
  eixoY,
  limitesRelevantes,
  localizar,
  marcasX,
  rodadaEm,
  segmentar,
  type Alvo,
} from "@/lib/grafico";

/**
 * Temperatura da água ao longo do tempo, com leitura exata de qualquer
 * momento.
 *
 * Toque, arraste ou passe o mouse no gráfico: uma linha vertical segue o dedo e
 * o valor, o dia e a hora aparecem **acima** do gráfico — onde o dedo não cobre.
 * O toque fixa o ponto; "Limpar" (ou Esc) solta. Pelo teclado, as setas andam
 * ponto a ponto. A rolagem vertical da página continua funcionando por cima do
 * gráfico (`touch-action: pan-y`): só o arraste na horizontal é do gráfico.
 *
 * O SVG tem o tamanho real do contêiner (medido, não esticado): texto e
 * marcadores saem no tamanho certo em qualquer largura de tela.
 */

const ALTURA = 168; // área de plotagem
const M = { t: 14, r: 12, l: 34 };
const FAIXA_Y = M.t + ALTURA + 8; // faixa "ventoinha ligada", logo abaixo
const FAIXA_H = 8;
const BASE_X = FAIXA_Y + FAIXA_H + 14; // linha de base dos rótulos do eixo X
const ALTURA_TOTAL = BASE_X + 5;

const arred = (n: number) => Math.round(n * 10) / 10;

/** Largura do contêiner, acompanhando rotação de tela e redimensionamento. */
function useLargura(ref: RefObject<HTMLElement | null>): number {
  const [largura, setLargura] = useState(320);
  useEffect(() => {
    const el = ref.current;
    if (el === null) return;
    const medir = () => setLargura(Math.max(240, Math.floor(el.getBoundingClientRect().width)));
    medir();
    const ro = new ResizeObserver(medir);
    ro.observe(el);
    return () => ro.disconnect();
  }, [ref]);
  return largura;
}

const fmtEixo = (v: number) => (Number.isInteger(v) ? String(v) : v.toFixed(1).replace(".", ","));

export interface GraficoProps {
  dados: FanHistoryResponse;
  /** Limites atuais da ventoinha — as linhas de referência. */
  limites: { liga: number; desliga: number } | null;
  /** Instante (segundos) fixado pelo toque, pelas estatísticas ou pela tabela. */
  fixo: number | null;
  onFixo: (t: number | null) => void;
  /** Rodada destacada (clicada na tabela). */
  selecionada: number | null;
}

export function GraficoTemperatura({ dados, limites, fixo, onFixo, selecionada }: GraficoProps) {
  const envoltorio = useRef<HTMLDivElement>(null);
  const largura = useLargura(envoltorio);
  const [pairando, setPairando] = useState<number | null>(null);
  const [arrastando, setArrastando] = useState(false);
  const sobreposicao = useRef<HTMLDivElement>(null);

  const t0 = Date.parse(dados.from) / 1000;
  const t1 = Date.parse(dados.to) / 1000;
  const bucket = dados.bucket_s;
  const gap = dados.gap_s;
  const pontos = dados.points;
  const runs = dados.runs;
  const plotW = Math.max(120, largura - M.l - M.r);

  const eixo = useMemo(() => {
    let min = Infinity;
    let max = -Infinity;
    for (const p of pontos) {
      if (p[2] < min) min = p[2];
      if (p[3] > max) max = p[3];
    }
    if (!Number.isFinite(min)) return null;
    const usaLimites = limitesRelevantes(min, max, limites);
    if (usaLimites && limites !== null) {
      min = Math.min(min, limites.desliga);
      max = Math.max(max, limites.liga);
    }
    return { ...eixoY(min, max), usaLimites };
  }, [pontos, limites]);

  const x = (t: number) => M.l + ((t - t0) / (t1 - t0)) * plotW;
  const y = (v: number) =>
    eixo === null ? 0 : M.t + (1 - (v - eixo.lo) / (eixo.hi - eixo.lo)) * ALTURA;

  // ── Camadas estáticas: só refazem quando os dados ou a largura mudam ────
  const camadas = useMemo(() => {
    if (eixo === null) return null;
    const px = (t: number) => M.l + ((t - t0) / (t1 - t0)) * plotW;
    const py = (v: number) => M.t + (1 - (v - eixo.lo) / (eixo.hi - eixo.lo)) * ALTURA;
    const centro = (p: TemperaturePoint) => px(p[0] + bucket / 2);

    const segmentos = segmentar(pontos, gap);

    const buracos = buracosDaSerie(pontos, bucket, gap, t0, t1);
    const marcas = marcasX(dados.range, t0, t1);
    // 24 px: o que cabe de um rótulo ("qua 21") sem encostar no do dia vizinho.
    const minSpan = (24 / plotW) * (t1 - t0);

    const comFaixa = bucket >= 300; // 15 min e 1 h: a variação dentro do intervalo importa

    const caminhoLinha = (seg: TemperaturePoint[]) =>
      seg.map((p, i) => `${i === 0 ? "M" : "L"}${arred(centro(p))},${arred(py(p[1]))}`).join("");
    const caminhoFaixa = (seg: TemperaturePoint[]) =>
      seg.map((p, i) => `${i === 0 ? "M" : "L"}${arred(centro(p))},${arred(py(p[3]))}`).join("") +
      [...seg]
        .reverse()
        .map((p) => `L${arred(centro(p))},${arred(py(p[2]))}`)
        .join("") +
      "Z";

    const selecao = runs.find((r) => r.id === selecionada);

    return (
      <>
        {/* Grade: hairline sólida, um tom acima da superfície */}
        {eixo.ticks.map((v) => (
          <g key={v}>
            <line className="g-grade" x1={M.l} x2={M.l + plotW} y1={arred(py(v))} y2={arred(py(v))} />
            <text className="g-rotulo" x={M.l - 6} y={arred(py(v)) + 3} textAnchor="end">
              {fmtEixo(v)}
            </text>
          </g>
        ))}
        <text className="g-rotulo" x={M.l - 6} y={M.t - 5} textAnchor="end">
          °C
        </text>

        {buracos.map(([a, b], i) => (
          <rect
            key={`b${i}`}
            className="g-buraco"
            x={arred(px(Math.max(a, t0)))}
            y={M.t}
            width={Math.max(1, arred(px(Math.min(b, t1)) - px(Math.max(a, t0))))}
            height={ALTURA}
          />
        ))}

        {selecao !== undefined ? (
          <rect
            className="g-selecao"
            x={arred(px(Math.max(Date.parse(selecao.started_at) / 1000, t0)))}
            y={M.t}
            width={Math.max(
              2,
              arred(
                px(Math.min(selecao.ended_at ? Date.parse(selecao.ended_at) / 1000 : t1, t1)) -
                  px(Math.max(Date.parse(selecao.started_at) / 1000, t0)),
              ),
            )}
            height={ALTURA + FAIXA_H + 8}
          />
        ) : null}

        {eixo.usaLimites && limites !== null ? (
          <>
            <line
              className="g-limite"
              x1={M.l}
              x2={M.l + plotW}
              y1={arred(py(limites.liga))}
              y2={arred(py(limites.liga))}
            />
            <text className="g-rotulo g-rotulo-limite" x={M.l + plotW - 2} y={arred(py(limites.liga)) - 4} textAnchor="end">
              liga {formatTemp(limites.liga)}
            </text>
            <line
              className="g-limite"
              x1={M.l}
              x2={M.l + plotW}
              y1={arred(py(limites.desliga))}
              y2={arred(py(limites.desliga))}
            />
            <text
              className="g-rotulo g-rotulo-limite"
              x={M.l + plotW - 2}
              y={arred(py(limites.desliga)) + 11}
              textAnchor="end"
            >
              desliga {formatTemp(limites.desliga)}
            </text>
          </>
        ) : null}

        {comFaixa
          ? segmentos.filter((s) => s.length > 1).map((s, i) => (
              <path key={`f${i}`} className="g-faixa" d={caminhoFaixa(s)} />
            ))
          : null}

        {segmentos.map((s, i) =>
          s.length > 1 ? (
            <path key={`l${i}`} className="g-linha" d={caminhoLinha(s)} />
          ) : (
            <circle key={`l${i}`} className="g-ponto-solto" cx={arred(centro(s[0]!))} cy={arred(py(s[0]![1]))} r={2.5} />
          ),
        )}

        {/* Faixa da ventoinha: o rastro de quando ela esteve ligada */}
        <rect className="g-trilho" x={M.l} y={FAIXA_Y} width={plotW} height={FAIXA_H} rx={4} />
        {runs.map((r) => {
          const ini = Math.max(Date.parse(r.started_at) / 1000, t0);
          const fim = Math.min(r.ended_at ? Date.parse(r.ended_at) / 1000 : t1, t1);
          if (fim < t0 || ini > t1) return null;
          return (
            <rect
              key={r.id}
              className={`g-ventoinha ${r.manual ? "manual" : ""} ${r.id === selecionada ? "sel" : ""}`}
              x={arred(px(ini))}
              y={FAIXA_Y}
              width={Math.max(2, arred(px(fim) - px(ini)))}
              height={FAIXA_H}
              rx={2}
            />
          );
        })}
        <text className="g-rotulo" x={M.l - 6} y={FAIXA_Y + FAIXA_H - 1} textAnchor="end">
          vent.
        </text>

        {marcas.map((m) =>
          m.limite && dados.range !== "24h" ? (
            <line
              key={`v${m.t}`}
              className="g-grade-v"
              x1={arred(px(m.t))}
              x2={arred(px(m.t))}
              y1={M.t}
              y2={FAIXA_Y + FAIXA_H}
            />
          ) : null,
        )}
        {marcas.map((m) =>
          // Um dia cortado pela borda é estreito demais para o rótulo: some, sem sobrepor o vizinho.
          m.span >= minSpan ? (
            <text key={`x${m.t}`} className="g-rotulo" x={arred(px(m.centro))} y={BASE_X} textAnchor="middle">
              {m.label}
            </text>
          ) : null,
        )}

        {/* Os extremos exatos do período, sobre a linha */}
        {dados.temperature
          ? ([
              [dados.temperature.min_at, dados.temperature.min],
              [dados.temperature.max_at, dados.temperature.max],
            ] as const).map(([quando, valor], i) => {
              const t = Date.parse(quando) / 1000;
              const bucketDoPonto = Math.floor(t / bucket) * bucket + bucket / 2;
              return (
                <circle
                  key={`e${i}`}
                  className="g-extremo"
                  cx={arred(px(bucketDoPonto))}
                  cy={arred(py(valor))}
                  r={4}
                />
              );
            })
          : null}
      </>
    );
    // `x`/`y` são derivadas das mesmas entradas; listar só as entradas evita refazer a cada render.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [dados, eixo, limites, plotW, selecionada]);

  // ── O que está sob o dedo ───────────────────────────────────────────────
  const ativo = pairando ?? fixo;
  const alvo: Alvo | null = ativo === null ? null : localizar(pontos, ativo, bucket, gap, t1);
  const pontoAtivo = alvo?.tipo === "ponto" ? pontos[alvo.i] : undefined;
  const rodada: FanRun | undefined =
    pontoAtivo !== undefined ? rodadaEm(runs, pontoAtivo[0] + bucket / 2, t1) : undefined;

  const tempoDoPonteiro = (clientX: number): number => {
    const r = sobreposicao.current?.getBoundingClientRect();
    if (r === undefined) return t0;
    const f = (clientX - r.left) / r.width;
    return t0 + Math.min(1, Math.max(0, f)) * (t1 - t0);
  };

  /*
   * Mouse: o ponteiro já pré-visualiza ao passar; o clique fixa.
   *
   * Toque: o dedo que só quer rolar a página não pode deixar um ponto fixado
   * para trás. Então o toque não fixa ao encostar — fixa ao soltar, se foi um
   * toque (sem arrastar), ou segue o dedo se o arraste é na horizontal. Um
   * arraste na vertical vira rolagem (`touch-action: pan-y`) e o navegador
   * cancela o ponteiro antes de qualquer fixação.
   */
  const toque = useRef<{ x: number; moveu: boolean } | null>(null);

  const aoApertar = (e: PointerEvent<HTMLDivElement>) => {
    try {
      e.currentTarget.setPointerCapture(e.pointerId);
    } catch {
      // O ponteiro já terminou (ou nunca esteve ativo): segue sem captura.
    }
    setArrastando(true);
    setPairando(null);
    if (e.pointerType === "mouse") {
      onFixo(tempoDoPonteiro(e.clientX));
    } else {
      toque.current = { x: e.clientX, moveu: false };
    }
  };
  const aoMover = (e: PointerEvent<HTMLDivElement>) => {
    if (!arrastando) {
      if (e.pointerType === "mouse") setPairando(tempoDoPonteiro(e.clientX));
      return;
    }
    if (e.pointerType === "mouse") {
      onFixo(tempoDoPonteiro(e.clientX));
      return;
    }
    const t = toque.current;
    if (t === null) return;
    if (!t.moveu && Math.abs(e.clientX - t.x) < 4) return;
    t.moveu = true;
    onFixo(tempoDoPonteiro(e.clientX));
  };
  const aoSoltar = (e: PointerEvent<HTMLDivElement>) => {
    setArrastando(false);
    const t = toque.current;
    toque.current = null;
    if (e.type === "pointerup" && e.pointerType !== "mouse" && t !== null && !t.moveu) {
      onFixo(tempoDoPonteiro(e.clientX)); // um toque: fixa onde encostou
    }
  };
  const aoSair = (e: PointerEvent<HTMLDivElement>) => {
    if (e.pointerType === "mouse") setPairando(null);
  };

  const irPara = (i: number) => {
    const j = Math.min(pontos.length - 1, Math.max(0, i));
    const p = pontos[j];
    if (p === undefined) return;
    setPairando(null);
    onFixo(p[0] + bucket / 2);
  };
  const aoTeclar = (e: KeyboardEvent<HTMLDivElement>) => {
    const atual = alvo?.tipo === "ponto" ? alvo.i : null;
    const passo = e.shiftKey ? 10 : 1;
    switch (e.key) {
      case "ArrowLeft":
        irPara((atual ?? pontos.length) - passo);
        break;
      case "ArrowRight":
        irPara((atual ?? pontos.length - 2) + passo);
        break;
      case "PageUp":
        irPara((atual ?? pontos.length) - 60);
        break;
      case "PageDown":
        irPara((atual ?? 0) + 60);
        break;
      case "Home":
        irPara(0);
        break;
      case "End":
        irPara(pontos.length - 1);
        break;
      case "Escape":
        setPairando(null);
        onFixo(null);
        break;
      default:
        return;
    }
    e.preventDefault();
  };

  const leitura = descrever({ alvo, ativo, pontos, bucket, t0, t1, gap, pontoAtivo });

  return (
    <div className="hist-grafico">
      <div className="hist-leitura" aria-live="off">
        <div className="hist-leitura-topo">
          <div className={`hist-leitura-valor ${leitura.valor === null ? "vazio" : ""}`}>
            {leitura.valor ?? "Sem leitura"}
            {leitura.valor !== null ? <span className="unid"> °C</span> : null}
          </div>
          <div className="hist-leitura-quando">{leitura.quando}</div>
        </div>
        <div className="hist-leitura-sub">
          <span>{leitura.sub}</span>
          {rodada !== undefined ? (
            <span className="chip chip-fan">
              Ventoinha ligada{rodada.manual ? " · manual" : ""}
            </span>
          ) : null}
          {fixo !== null ? (
            <button
              type="button"
              className="hist-limpar"
              onClick={() => {
                setPairando(null);
                onFixo(null);
              }}
            >
              Limpar
            </button>
          ) : null}
        </div>
      </div>

      <div className="hist-grafico-caixa" ref={envoltorio}>
        <svg
          className="hist-svg"
          width={largura}
          height={ALTURA_TOTAL}
          viewBox={`0 0 ${largura} ${ALTURA_TOTAL}`}
          role="img"
          aria-label={resumoAcessivel(dados)}
        >
          {camadas}

          {alvo?.tipo === "ponto" && pontoAtivo !== undefined ? (
            <g>
              <line
                className="g-cursor"
                x1={arred(x(pontoAtivo[0] + bucket / 2))}
                x2={arred(x(pontoAtivo[0] + bucket / 2))}
                y1={M.t}
                y2={FAIXA_Y + FAIXA_H}
              />
              <circle
                className="g-marcador"
                cx={arred(x(pontoAtivo[0] + bucket / 2))}
                cy={arred(y(pontoAtivo[1]))}
                r={5}
              />
            </g>
          ) : null}
          {alvo?.tipo === "buraco" && ativo !== null ? (
            <line
              className="g-cursor buraco"
              x1={arred(x(ativo))}
              x2={arred(x(ativo))}
              y1={M.t}
              y2={FAIXA_Y + FAIXA_H}
            />
          ) : null}
        </svg>

        {/* A área sensível é HTML: foco, teclado e ponteiro funcionam igual em qualquer navegador */}
        <div
          ref={sobreposicao}
          className="hist-alvo"
          style={{ left: M.l, top: M.t, width: plotW, height: ALTURA }}
          tabIndex={0}
          role="slider"
          aria-label="Percorrer a temperatura no tempo"
          aria-orientation="horizontal"
          aria-valuemin={Math.round(t0)}
          aria-valuemax={Math.round(t1)}
          aria-valuenow={Math.round(ativo ?? t1)}
          aria-valuetext={`${leitura.valor === null ? "Sem leitura" : `${leitura.valor} graus`}, ${leitura.quando}`}
          onPointerDown={aoApertar}
          onPointerMove={aoMover}
          onPointerUp={aoSoltar}
          onPointerCancel={aoSoltar}
          onPointerLeave={aoSair}
          onKeyDown={aoTeclar}
        />
      </div>
    </div>
  );
}

// ── Texto da leitura ──────────────────────────────────────────────────────

interface Leitura {
  valor: string | null;
  quando: string;
  sub: string;
}

function descrever(a: {
  alvo: Alvo | null;
  ativo: number | null;
  pontos: TemperaturePoint[];
  bucket: number;
  t0: number;
  t1: number;
  gap: number;
  pontoAtivo: TemperaturePoint | undefined;
}): Leitura {
  const { alvo, pontos, bucket, t0, t1, gap, pontoAtivo } = a;

  if (alvo?.tipo === "ponto" && pontoAtivo !== undefined) {
    const ini = new Date(pontoAtivo[0] * 1000);
    const fim = new Date((pontoAtivo[0] + bucket) * 1000);
    const quando =
      bucket <= 120
        ? `${formatDia(ini)} · ${formatHora(ini)}`
        : `${formatDia(ini)} · ${formatHora(ini)}–${formatHora(fim)}`;
    const [, avg, min, max] = pontoAtivo;
    const variou = Math.abs(max - min) >= 0.05;
    return {
      valor: formatTemp(avg),
      quando,
      sub: variou
        ? `média de ${bucket >= 3600 ? "1 h" : `${bucket / 60} min`} · mín ${formatTemp(min)} · máx ${formatTemp(max)}`
        : bucket > 120
          ? `média de ${bucket >= 3600 ? "1 h" : `${bucket / 60} min`}`
          : "",
    };
  }

  if (alvo?.tipo === "buraco") {
    const de = new Date(Math.max(alvo.de, t0) * 1000);
    const ate = new Date(Math.min(alvo.ate, t1) * 1000);
    return {
      valor: null,
      quando: `${formatDia(de)} · ${formatHora(de)}–${formatHora(ate)}`,
      sub: "o aquário ficou sem contato com o servidor",
    };
  }

  // Repouso: a última leitura, e como chegar às outras.
  const ultimo = pontos.at(-1);
  if (ultimo === undefined) return { valor: null, quando: "", sub: "" };
  const idade = Math.floor(t1 - (ultimo[0] + bucket));
  if (t1 - ultimo[0] <= gap) {
    return {
      valor: formatTemp(ultimo[1]),
      quando: `última leitura · ${idade < 60 ? "agora" : secondsAgo(idade)}`,
      sub: "toque ou arraste no gráfico para ver cada momento",
    };
  }
  const quando = new Date(ultimo[0] * 1000);
  return {
    valor: null,
    quando: `sem leitura ${secondsAgo(Math.max(0, idade))}`,
    sub: `última: ${formatTemp(ultimo[1])} °C · ${formatDia(quando)} ${formatHora(quando)}`,
  };
}

function resumoAcessivel(d: FanHistoryResponse): string {
  const periodo = d.range === "24h" ? "nas últimas 24 horas" : d.range === "7d" ? "nos últimos 7 dias" : "nos últimos 30 dias";
  if (d.temperature === null) return `Temperatura da água ${periodo}: sem leituras.`;
  return `Temperatura da água ${periodo}: mínima ${formatTemp(d.temperature.min)}, média ${formatTemp(d.temperature.avg)}, máxima ${formatTemp(d.temperature.max)} graus. Use a área abaixo para percorrer cada momento.`;
}
