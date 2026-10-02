"use client";

import type { ReportPoint } from "@bettacare/contract";
import { useEffect, useState } from "react";

import { fetchReport } from "@/lib/api";
import { formatDuration, formatTemp } from "@/lib/format";

/**
 * Histórico de temperatura e de tempo ligado — o que era a aba Relatórios,
 * agora junto da temperatura, que é onde a pergunta "como andou a água?"
 * nasce. A planilha completa fica na aba Registros.
 *
 * Alimentado por `telemetry_hourly`, agregado a cada hora cheia: o gráfico
 * vai até a última hora fechada.
 *
 * O gráfico é SVG escrito à mão. Uma biblioteca de charts custaria mais em
 * bundle do que estas linhas, para desenhar uma faixa e uma linha.
 */
export function TempHistory() {
  const [dias, setDias] = useState(1);
  const [pontos, setPontos] = useState<ReportPoint[]>([]);
  const [carregando, setCarregando] = useState(true);
  const [erro, setErro] = useState(false);

  useEffect(() => {
    const ac = new AbortController();
    setCarregando(true);
    fetchReport(dias, ac.signal)
      .then((p) => {
        setPontos(p);
        setErro(false);
      })
      .catch((e: unknown) => {
        if ((e as Error).name !== "AbortError") setErro(true);
      })
      .finally(() => setCarregando(false));
    return () => ac.abort();
  }, [dias]);

  const comTemp = pontos.filter((p) => p.temp_avg !== null);
  const luzTotal = pontos.reduce((a, p) => a + p.light_minutes, 0);
  const fanTotal = pontos.reduce((a, p) => a + p.fan_minutes, 0);
  const tMin = comTemp.length ? Math.min(...comTemp.map((p) => p.temp_min ?? 99)) : null;
  const tMax = comTemp.length ? Math.max(...comTemp.map((p) => p.temp_max ?? -99)) : null;
  const tMed = comTemp.length
    ? comTemp.reduce((a, p) => a + (p.temp_avg ?? 0), 0) / comTemp.length
    : null;

  return (
    <div className="chart-card">
      <div className="row-between" style={{ marginBottom: 12 }}>
        <div className="chart-title" style={{ marginBottom: 0 }}>
          Histórico
        </div>
        <div className="segmentado pequeno" role="tablist" aria-label="Período do histórico">
          {[1, 7, 30].map((d) => (
            <button
              key={d}
              role="tab"
              aria-selected={dias === d}
              className={dias === d ? "ativo" : ""}
              onClick={() => setDias(d)}
            >
              {d === 1 ? "24 h" : `${d} dias`}
            </button>
          ))}
        </div>
      </div>

      {carregando && pontos.length === 0 ? <div className="empty">Carregando…</div> : null}
      {erro ? <div className="empty">Não foi possível carregar o histórico.</div> : null}

      {!carregando && !erro && pontos.length === 0 ? (
        <div className="empty">
          Ainda não há horas completas neste período. O histórico é agregado a cada
          hora cheia.
        </div>
      ) : null}

      {comTemp.length > 1 ? (
        <>
          <TempChart pontos={comTemp} />
          <div className="stat-grid">
            <div className="stat">
              <div className="stat-val">{formatTemp(tMin)}</div>
              <div className="stat-lbl">mínima °C</div>
            </div>
            <div className="stat">
              <div className="stat-val">{formatTemp(tMed)}</div>
              <div className="stat-lbl">média °C</div>
            </div>
            <div className="stat">
              <div className="stat-val">{formatTemp(tMax)}</div>
              <div className="stat-lbl">máxima °C</div>
            </div>
          </div>
        </>
      ) : null}

      {pontos.length > 0 ? (
        <div className="stat-grid" style={{ gridTemplateColumns: "1fr 1fr", marginTop: 16 }}>
          <div className="stat">
            <div className="stat-val" style={{ color: "var(--c-on)" }}>
              {formatDuration(dias === 1 ? fanTotal : Math.round(fanTotal / dias))}
            </div>
            <div className="stat-lbl">ventoinha ligada{dias === 1 ? "" : " por dia"}</div>
          </div>
          <div className="stat">
            <div className="stat-val" style={{ color: "var(--c-amber)" }}>
              {formatDuration(dias === 1 ? luzTotal : Math.round(luzTotal / dias))}
            </div>
            <div className="stat-lbl">luz acesa{dias === 1 ? "" : " por dia"}</div>
          </div>
        </div>
      ) : null}
    </div>
  );
}

/** Faixa mínima–máxima com a linha da média por cima. */
function TempChart({ pontos }: { pontos: ReportPoint[] }) {
  const W = 320;
  const H = 120;
  const P = 4;

  const valores = pontos.flatMap((p) => [p.temp_min ?? 0, p.temp_max ?? 0]);
  const lo = Math.floor(Math.min(...valores) - 0.5);
  const hi = Math.ceil(Math.max(...valores) + 0.5);
  const span = hi - lo || 1;

  const x = (i: number) => P + (i / (pontos.length - 1)) * (W - 2 * P);
  const y = (v: number) => H - P - ((v - lo) / span) * (H - 2 * P);

  /*
   * O polígono da faixa vai pela máxima da esquerda para a direita e volta
   * pela mínima. O `reverse()` precisa vir **depois** do map, sobre as
   * coordenadas já formadas: invertendo o índice dentro do map, cada ponto
   * ganharia o x de uma posição e o y de outra, e a faixa sairia embaralhada
   * em vez de acompanhar a linha da média.
   */
  const areaTopo = pontos.map((p, i) => `${x(i)},${y(p.temp_max ?? 0)}`).join(" ");
  const areaBase = pontos
    .map((p, i) => `${x(i)},${y(p.temp_min ?? 0)}`)
    .reverse()
    .join(" ");
  const media = pontos.map((p, i) => `${x(i)},${y(p.temp_avg ?? 0)}`).join(" ");

  return (
    <svg
      className="chart-svg"
      viewBox={`0 0 ${W} ${H}`}
      preserveAspectRatio="none"
      role="img"
      aria-label={`Temperatura entre ${lo} e ${hi} graus`}
    >
      <polygon points={`${areaTopo} ${areaBase}`} fill="rgba(76,195,247,0.14)" stroke="none" />
      <polyline
        points={media}
        fill="none"
        stroke="var(--c-accent)"
        strokeWidth={1.6}
        strokeLinejoin="round"
        vectorEffect="non-scaling-stroke"
      />
      <text x={P} y={12} fill="var(--c-text-3)" fontSize={9}>
        {hi} °C
      </text>
      <text x={P} y={H - 2} fill="var(--c-text-3)" fontSize={9}>
        {lo} °C
      </text>
    </svg>
  );
}
