"use client";

import type { ReportPoint } from "@bettacare/contract";
import { useEffect, useState } from "react";

import { fetchReport } from "@/lib/api";
import { formatDuration } from "@/lib/format";

/**
 * Relatórios.
 *
 * Alimentados por `telemetry_hourly`, não pela telemetria bruta — então
 * continuam funcionando sobre períodos cuja telemetria já foi descartada pela
 * purga de tamanho. Era o que não dava para fazer com MQTT retido, e é a razão
 * de o banco existir.
 *
 * O gráfico é SVG escrito à mão. Uma biblioteca de charts custaria mais em
 * bundle do que estas trinta linhas, para desenhar uma faixa e uma linha.
 */
export function Report() {
  const [dias, setDias] = useState(7);
  const [pontos, setPontos] = useState<ReportPoint[]>([]);
  const [carregando, setCarregando] = useState(true);

  useEffect(() => {
    const ac = new AbortController();
    setCarregando(true);
    fetchReport(dias, ac.signal)
      .then(setPontos)
      .catch(() => undefined)
      .finally(() => setCarregando(false));
    return () => ac.abort();
  }, [dias]);

  const comTemp = pontos.filter((p) => p.temp_avg !== null);
  const luzTotal = pontos.reduce((a, p) => a + p.light_minutes, 0);
  const fanTotal = pontos.reduce((a, p) => a + p.fan_minutes, 0);
  const tMin = comTemp.length ? Math.min(...comTemp.map((p) => p.temp_min ?? 99)) : null;
  const tMax = comTemp.length ? Math.max(...comTemp.map((p) => p.temp_max ?? -99)) : null;

  return (
    <>
      <div className="chips">
        {[1, 7, 30].map((d) => (
          <button
            key={d}
            className={`chip ${dias === d ? "active" : ""}`}
            onClick={() => setDias(d)}
          >
            {d === 1 ? "24 horas" : `${d} dias`}
          </button>
        ))}
      </div>

      {carregando ? <div className="empty">Carregando…</div> : null}

      {!carregando && pontos.length === 0 ? (
        <div className="empty">
          Ainda não há histórico agregado para este período. O rollup roda uma
          vez por dia, de madrugada.
        </div>
      ) : null}

      {!carregando && comTemp.length > 1 ? (
        <div className="chart-card">
          <div className="chart-title">Temperatura</div>
          <TempChart pontos={comTemp} />
          <div className="stat-grid">
            <div className="stat">
              <div className="stat-val">{tMin?.toFixed(1) ?? "—"}</div>
              <div className="stat-lbl">mínima °C</div>
            </div>
            <div className="stat">
              <div className="stat-val">
                {comTemp.length
                  ? (
                      comTemp.reduce((a, p) => a + (p.temp_avg ?? 0), 0) /
                      comTemp.length
                    ).toFixed(1)
                  : "—"}
              </div>
              <div className="stat-lbl">média °C</div>
            </div>
            <div className="stat">
              <div className="stat-val">{tMax?.toFixed(1) ?? "—"}</div>
              <div className="stat-lbl">máxima °C</div>
            </div>
          </div>
        </div>
      ) : null}

      {!carregando && pontos.length > 0 ? (
        <div className="chart-card">
          <div className="chart-title">Tempo ligado</div>
          <div className="stat-grid" style={{ gridTemplateColumns: "1fr 1fr" }}>
            <div className="stat">
              <div className="stat-val" style={{ color: "var(--c-amber)" }}>
                {formatDuration(luzTotal)}
              </div>
              <div className="stat-lbl">luminária</div>
            </div>
            <div className="stat">
              <div className="stat-val" style={{ color: "var(--c-on)" }}>
                {formatDuration(fanTotal)}
              </div>
              <div className="stat-lbl">ventoinha</div>
            </div>
          </div>
          <div className="c-sub" style={{ textAlign: "center", marginTop: 12 }}>
            {dias === 1
              ? "nas últimas 24 horas"
              : `média de ${formatDuration(Math.round(luzTotal / dias))} de luz por dia`}
          </div>
        </div>
      ) : null}
    </>
  );
}

/** Faixa min–máx com a linha da média por cima. */
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
      <polygon
        points={`${areaTopo} ${areaBase}`}
        fill="rgba(76,195,247,0.14)"
        stroke="none"
      />
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
