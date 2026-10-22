"use client";

import type { DeviceConfig, ReportPoint } from "@bettacare/contract";
import { useEffect, useMemo, useRef, useState, type PointerEvent } from "react";

import { fetchReport } from "@/lib/api";
import { formatDia, formatDuration, formatHora } from "@/lib/format";

type Dias = 1 | 7 | 30;

const PERIODOS: ReadonlyArray<{ id: Dias; rotulo: string }> = [
  { id: 1, rotulo: "24 h" },
  { id: 7, rotulo: "7 dias" },
  { id: 30, rotulo: "30 dias" },
];

/** O rollup fecha a cada hora cheia; o que passou disso só entra na hora seguinte. */
const REFRESH_MS = 5 * 60_000;

interface Fatia {
  inicio: Date;
  /** Minutos de luz acesa; nulo = nenhuma linha do rollup (sem dado, não "zero"). */
  minutos: number | null;
}

/** Quanto tempo a luminária fica acesa por dia, pelo horário programado (minutos). */
function minutosProgramados(config: DeviceConfig | null): number | null {
  if (config === null) return null;
  const [h1, m1] = config.light_on_time.split(":").map(Number);
  const [h2, m2] = config.light_off_time.split(":").map(Number);
  if ([h1, m1, h2, m2].some((n) => n === undefined || Number.isNaN(n))) return null;
  const dif = (h2! * 60 + m2!) - (h1! * 60 + m1!);
  return ((dif % 1440) + 1440) % 1440;
}

/** As fatias do período — 24 horas ou N dias, no relógio local, terminando agora. */
function fatiasDe(dias: Dias, agora: Date): Date[] {
  const inicios: Date[] = [];
  if (dias === 1) {
    const base = new Date(agora);
    base.setMinutes(0, 0, 0);
    for (let i = 23; i >= 0; i--) {
      const d = new Date(base);
      d.setHours(base.getHours() - i);
      inicios.push(d);
    }
  } else {
    const hoje = new Date(agora);
    hoje.setHours(0, 0, 0, 0);
    for (let i = dias - 1; i >= 0; i--) {
      const d = new Date(hoje);
      d.setDate(hoje.getDate() - i);
      inicios.push(d);
    }
  }
  return inicios;
}

function agrupar(linhas: ReportPoint[], inicios: Date[], dias: Dias): Fatia[] {
  const fatias: Fatia[] = inicios.map((inicio) => ({ inicio, minutos: null }));
  const indice = new Map<number, number>();
  inicios.forEach((d, i) => indice.set(d.getTime(), i));
  for (const l of linhas) {
    const h = new Date(l.hour);
    if (dias !== 1) h.setHours(0, 0, 0, 0);
    const i = indice.get(h.getTime());
    if (i === undefined) continue;
    const f = fatias[i]!;
    f.minutos = (f.minutos ?? 0) + l.light_minutes;
  }
  return fatias;
}

/**
 * Quanto tempo a luminária ficou acesa — por hora nas últimas 24 h, por dia em
 * 7 e 30 dias. Era uma linha solta no histórico da ventoinha; a pergunta é da
 * luminária, e aqui ela se responde com a rotina inteira à vista (e o horário
 * programado como referência).
 */
export function HistoricoLuz({
  ativo,
  config,
}: {
  ativo: boolean;
  config: DeviceConfig | null;
}) {
  const [dias, setDias] = useState<Dias>(1);
  const [linhas, setLinhas] = useState<{ dias: Dias; itens: ReportPoint[]; agora: number } | null>(null);
  const [erro, setErro] = useState(false);
  const [carregando, setCarregando] = useState(false);

  useEffect(() => {
    if (!ativo) return;
    let encerrado = false;
    let ac: AbortController | null = null;

    const carregar = () => {
      ac?.abort();
      ac = new AbortController();
      const agora = new Date();
      const inicios = fatiasDe(dias, agora);
      setCarregando(true);
      fetchReport(inicios[0]!, agora, ac.signal)
        .then((itens) => {
          if (encerrado) return;
          setLinhas({ dias, itens, agora: agora.getTime() });
          setErro(false);
        })
        .catch((e: unknown) => {
          if (!encerrado && (e as Error).name !== "AbortError") setErro(true);
        })
        .finally(() => {
          if (!encerrado) setCarregando(false);
        });
    };

    const seVisivel = () => {
      if (document.visibilityState === "visible") carregar();
    };
    carregar();
    const t = setInterval(seVisivel, REFRESH_MS);
    document.addEventListener("visibilitychange", seVisivel);
    return () => {
      encerrado = true;
      ac?.abort();
      clearInterval(t);
      document.removeEventListener("visibilitychange", seVisivel);
    };
  }, [dias, ativo]);

  const trocando = carregando && linhas !== null && linhas.dias !== dias;

  return (
    <section className="chart-card hist-card" aria-label="Tempo aceso da luminária">
      <div className="hist-filtro">
        <h2 className="chart-title">Tempo aceso</h2>
        <div className="segmentado pequeno" role="tablist" aria-label="Período">
          {PERIODOS.map((p) => (
            <button
              key={p.id}
              type="button"
              role="tab"
              aria-selected={dias === p.id}
              className={dias === p.id ? "ativo" : ""}
              onClick={() => setDias(p.id)}
            >
              {p.rotulo}
            </button>
          ))}
        </div>
      </div>

      {linhas === null ? (
        <div className="empty">{erro ? "Não foi possível carregar o histórico." : "Carregando…"}</div>
      ) : (
        <div className={trocando ? "stale" : ""} aria-busy={trocando}>
          {/* `key` = período: dados novos começam sem barra escolhida */}
          <Conteudo key={linhas.dias} linhas={linhas} config={config} />
        </div>
      )}
    </section>
  );
}

function Conteudo({
  linhas,
  config,
}: {
  linhas: { dias: Dias; itens: ReportPoint[]; agora: number };
  config: DeviceConfig | null;
}) {
  const periodo = linhas.dias;
  const [escolhida, setEscolhida] = useState<number | null>(null);

  const fatias = useMemo(
    () => agrupar(linhas.itens, fatiasDe(periodo, new Date(linhas.agora)), periodo),
    [linhas, periodo],
  );
  const programado = minutosProgramados(config);

  // O número grande: total das 24 h; ou a média dos dias completos (o de hoje
  // ainda está acontecendo e puxaria a média para baixo).
  const destaque = useMemo(() => {
    if (fatias.length === 0) return null;
    if (periodo === 1) {
      const total = fatias.reduce((a, f) => a + (f.minutos ?? 0), 0);
      return { minutos: total, rotulo: "luz acesa nas últimas 24 h" };
    }
    const completos = fatias.slice(0, -1).filter((f) => f.minutos !== null);
    if (completos.length === 0) {
      const hoje = fatias.at(-1)?.minutos ?? null;
      return hoje === null ? null : { minutos: hoje, rotulo: "luz acesa hoje, até a última hora cheia" };
    }
    const media = completos.reduce((a, f) => a + (f.minutos ?? 0), 0) / completos.length;
    return {
      minutos: Math.round(media),
      rotulo: `luz acesa por dia · média de ${completos.length} ${completos.length === 1 ? "dia completo" : "dias completos"}`,
    };
  }, [fatias, periodo]);

  const escolhidaFatia = escolhida !== null ? fatias[escolhida] : undefined;

  return (
    <>
      <div className="hist-leitura">
        <div className="hist-leitura-topo">
          <div className="hist-leitura-valor">
            {escolhidaFatia !== undefined
              ? escolhidaFatia.minutos === null
                ? "Sem dado"
                : formatDuration(escolhidaFatia.minutos)
              : destaque !== null
                ? formatDuration(destaque.minutos)
                : "—"}
          </div>
          <div className="hist-leitura-quando">
            {escolhidaFatia !== undefined ? rotuloDaFatia(escolhidaFatia, periodo) : ""}
          </div>
        </div>
        <div className="hist-leitura-sub">
          {escolhidaFatia !== undefined
            ? periodo === 1
              ? "acesa nesta hora"
              : "acesa neste dia"
            : (destaque?.rotulo ?? "sem dados no período")}
          {escolhida !== null ? (
            <button type="button" className="hist-limpar" onClick={() => setEscolhida(null)}>
              Limpar
            </button>
          ) : null}
        </div>
      </div>

      <Barras
        fatias={fatias}
        periodo={periodo}
        programado={periodo === 1 ? null : programado}
        escolhida={escolhida}
        onEscolher={setEscolhida}
      />

      <p className="chart-nota">
        {periodo === 1 ? "Cada barra é uma hora; " : "Cada barra é um dia; "}
        atualizado a cada hora cheia
        {programado !== null && periodo !== 1
          ? `. A linha tracejada é o horário programado (${formatDuration(programado)} por dia).`
          : "."}{" "}
        Dia sem barra é dia sem dados.
      </p>
    </>
  );
}

function rotuloDaFatia(f: Fatia, periodo: Dias): string {
  if (periodo === 1) {
    const fim = new Date(f.inicio.getTime() + 3600_000);
    return `${formatDia(f.inicio)} · ${formatHora(f.inicio)}–${formatHora(fim)}`;
  }
  return formatDia(f.inicio);
}

// ── Barras ────────────────────────────────────────────────────────────────

/** Barra de largura `w` centrada em `cx`, saindo de `base`: reta embaixo, ponta de até 4 px arredondada. */
function caminhoBarra(cx: number, w: number, base: number, altura: number): string {
  const r = Math.min(4, w / 2, altura);
  const x0 = cx - w / 2;
  const x1 = cx + w / 2;
  const topo = base - altura;
  return `M${x0},${base} V${topo + r} Q${x0},${topo} ${x0 + r},${topo} H${x1 - r} Q${x1},${topo} ${x1},${topo + r} V${base} Z`;
}

const ALTURA = 110;
const M = { t: 10, b: 20, l: 30, r: 6 };

function Barras({
  fatias,
  periodo,
  programado,
  escolhida,
  onEscolher,
}: {
  fatias: Fatia[];
  periodo: Dias;
  /** Minutos por dia programados; nulo = sem linha de referência. */
  programado: number | null;
  escolhida: number | null;
  onEscolher: (i: number | null) => void;
}) {
  const caixa = useRef<HTMLDivElement>(null);
  const [largura, setLargura] = useState(320);
  useEffect(() => {
    const el = caixa.current;
    if (el === null) return;
    const medir = () => setLargura(Math.max(240, Math.floor(el.getBoundingClientRect().width)));
    medir();
    const ro = new ResizeObserver(medir);
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  const n = fatias.length;
  const plotW = largura - M.l - M.r;
  const slot = plotW / Math.max(1, n);
  const largBarra = Math.min(24, Math.max(3, slot - 2)); // ≤ 24 px; 2 px de ar entre vizinhas

  // Escala: de zero ao maior valor (ou ao programado), em horas redondas.
  const maxMin = Math.max(60, programado ?? 0, ...fatias.map((f) => f.minutos ?? 0));
  const passoH = periodo === 1 ? 0.5 : maxMin > 12 * 60 ? 6 : maxMin > 8 * 60 ? 4 : 2;
  const topoMin = Math.ceil(maxMin / 60 / passoH) * passoH * 60;
  const yPx = (min: number) => M.t + (1 - min / topoMin) * ALTURA;

  const ticks: number[] = [];
  for (let m = 0; m <= topoMin; m += passoH * 60) ticks.push(m);

  const indiceNoX = (clientX: number): number => {
    const r = caixa.current?.getBoundingClientRect();
    if (r === undefined) return 0;
    return Math.min(n - 1, Math.max(0, Math.floor((clientX - r.left - M.l) / slot)));
  };
  const arrastando = useRef(false);
  const aoApertar = (e: PointerEvent<HTMLDivElement>) => {
    try {
      e.currentTarget.setPointerCapture(e.pointerId);
    } catch {
      // O ponteiro já terminou: segue sem captura.
    }
    arrastando.current = true;
    onEscolher(indiceNoX(e.clientX));
  };
  const aoMover = (e: PointerEvent<HTMLDivElement>) => {
    if (arrastando.current || e.pointerType === "mouse") onEscolher(indiceNoX(e.clientX));
  };
  const aoSoltar = () => {
    arrastando.current = false;
  };

  const rotuloX = (f: Fatia, i: number): string | null => {
    if (periodo === 1) return f.inicio.getHours() % 6 === 0 ? `${String(f.inicio.getHours()).padStart(2, "0")}h` : null;
    if (periodo === 7) return `${["dom", "seg", "ter", "qua", "qui", "sex", "sáb"][f.inicio.getDay()]} ${f.inicio.getDate()}`;
    return (n - 1 - i) % 5 === 0 ? `${String(f.inicio.getDate()).padStart(2, "0")}/${String(f.inicio.getMonth() + 1).padStart(2, "0")}` : null;
  };

  return (
    <div className="hist-grafico-caixa" ref={caixa}>
      <svg
        className="hist-svg"
        width={largura}
        height={M.t + ALTURA + M.b}
        viewBox={`0 0 ${largura} ${M.t + ALTURA + M.b}`}
        role="img"
        aria-label={`Tempo de luz acesa, ${periodo === 1 ? "hora a hora nas últimas 24 horas" : `dia a dia nos últimos ${periodo} dias`}.`}
      >
        {ticks.map((m) => (
          <g key={m}>
            <line className="g-grade" x1={M.l} x2={M.l + plotW} y1={yPx(m)} y2={yPx(m)} />
            <text className="g-rotulo" x={M.l - 6} y={yPx(m) + 3} textAnchor="end">
              {periodo === 1 ? `${m} min` : `${m / 60} h`}
            </text>
          </g>
        ))}

        {programado !== null ? (
          <line className="g-limite" x1={M.l} x2={M.l + plotW} y1={yPx(programado)} y2={yPx(programado)} />
        ) : null}

        {fatias.map((f, i) => {
          const cx = M.l + slot * i + slot / 2;
          const rotulo = rotuloX(f, i);
          const altura = f.minutos === null ? 0 : (f.minutos / topoMin) * ALTURA;
          return (
            <g key={f.inicio.getTime()}>
              {f.minutos !== null && altura > 0 ? (
                // Base reta, ponta de 4 px arredondada: a barra cresce de uma única linha de base.
                <path className={`g-barra ${i === escolhida ? "sel" : ""}`} d={caminhoBarra(cx, largBarra, M.t + ALTURA, altura)} />
              ) : f.minutos === 0 ? (
                <rect className="g-barra zero" x={cx - largBarra / 2} y={M.t + ALTURA - 2} width={largBarra} height={2} />
              ) : null}
              {rotulo !== null ? (
                <text className="g-rotulo" x={cx} y={M.t + ALTURA + 14} textAnchor="middle">
                  {rotulo}
                </text>
              ) : null}
            </g>
          );
        })}
      </svg>

      <div
        className="hist-alvo"
        style={{ left: M.l, top: M.t, width: plotW, height: ALTURA }}
        role="group"
        aria-label="Toque numa barra para ver o tempo aceso"
        onPointerDown={aoApertar}
        onPointerMove={aoMover}
        onPointerUp={aoSoltar}
        onPointerCancel={aoSoltar}
      />
    </div>
  );
}
