"use client";

import type { FanHistoryResponse, FanRun } from "@bettacare/contract";
import { Fragment, useEffect, useState } from "react";

import { formatDia, formatDurationS, formatHora, formatTemp } from "@/lib/format";

/** Rodadas mostradas de cada vez — o resto vem em "Mostrar mais". */
const PAGINA = 12;
/** Variação abaixo disto é ruído do sensor, não esfriamento nem aquecimento. */
const DELTA_MINIMO = 0.3;

/** Relógio que anda sozinho — a rodada em curso precisa contar o tempo na tela. */
function useAgora(ms: number): number {
  const [agora, setAgora] = useState(() => Date.now());
  useEffect(() => {
    const t = setInterval(() => setAgora(Date.now()), ms);
    return () => clearInterval(t);
  }, [ms]);
  return agora;
}

const sinal = (graus: number) => `${graus > 0 ? "−" : "+"}${formatTemp(Math.abs(graus))}`;

/**
 * Quando a ventoinha ligou, quando desligou, quanto tempo girou e o que fez com
 * a temperatura — o que responde "ela está dando conta?".
 */
export function CartaoAcionamentos({
  dados,
  limites,
  online,
  celsiusAgora,
  selecionada,
  onSelecionar,
}: {
  dados: FanHistoryResponse;
  limites: { liga: number; desliga: number } | null;
  online: boolean;
  /** Temperatura de agora: é a "de saída" provisória da rodada em curso. */
  celsiusAgora: number | null;
  selecionada: number | null;
  onSelecionar: (id: number | null) => void;
}) {
  const agora = useAgora(30_000);
  const [mostrar, setMostrar] = useState(PAGINA);
  const { runs, summary } = dados;
  const periodoS = (Date.parse(dados.to) - Date.parse(dados.from)) / 1000;
  const pct = periodoS > 0 ? (summary.on_seconds / periodoS) * 100 : 0;

  return (
    <section className="chart-card hist-card" aria-label="Acionamentos da ventoinha">
      <div className="chart-title">Acionamentos da ventoinha</div>

      <div className="hist-kpis">
        <div className="kpi">
          <div className="kpi-val">{summary.runs}</div>
          <div className="kpi-lbl">{summary.runs === 1 ? "acionamento" : "acionamentos"}</div>
        </div>
        <div className="kpi">
          <div className="kpi-val">{summary.on_seconds === 0 ? "0 min" : formatDurationS(summary.on_seconds)}</div>
          <div className="kpi-lbl">
            ligada{summary.on_seconds > 0 ? ` · ${pct < 0.1 ? "<0,1" : pct.toFixed(1).replace(".", ",")} % do tempo` : ""}
          </div>
        </div>
        <div className="kpi">
          <div className="kpi-val">
            {summary.avg_duration_s !== null ? formatDurationS(summary.avg_duration_s) : "—"}
          </div>
          <div className="kpi-lbl">duração média</div>
        </div>
        <div className="kpi">
          <div className="kpi-val">
            {summary.avg_drop_c !== null ? (
              <>
                {sinal(summary.avg_drop_c)}
                <small> °C</small>
              </>
            ) : (
              "—"
            )}
          </div>
          <div className="kpi-lbl">variação média por acionamento automático</div>
        </div>
      </div>

      {runs.length === 0 ? (
        <div className="empty hist-vazio">{mensagemSemRodadas(dados, limites)}</div>
      ) : (
        <>
          <div className="hist-tabela-rolagem">
            <table className="tabela-acionamentos">
              <thead>
                <tr>
                  <th scope="col">Ligou</th>
                  <th scope="col">Desligou</th>
                  <th scope="col">Tempo</th>
                  <th scope="col">Temperatura °C</th>
                </tr>
              </thead>
              <tbody>
                {runs.slice(0, mostrar).map((r, i, visiveis) => {
                  const ini = new Date(r.started_at);
                  const anterior = visiveis[i - 1];
                  const novoDia =
                    anterior === undefined ||
                    new Date(anterior.started_at).toDateString() !== ini.toDateString();
                  return (
                    <Fragment key={r.id}>
                      {novoDia ? (
                        <tr className="dia">
                          <th scope="colgroup" colSpan={4}>
                            {formatDia(ini)}
                          </th>
                        </tr>
                      ) : null}
                      <Linha
                        run={r}
                        agora={agora}
                        online={online}
                        celsiusAgora={celsiusAgora}
                        selecionada={r.id === selecionada}
                        onSelecionar={() => onSelecionar(r.id === selecionada ? null : r.id)}
                      />
                    </Fragment>
                  );
                })}
              </tbody>
            </table>
          </div>

          {runs.length > mostrar ? (
            <button type="button" className="load-more" onClick={() => setMostrar((n) => n + PAGINA)}>
              Mostrar mais ({runs.length - mostrar})
            </button>
          ) : null}
          {dados.runs_total > runs.length ? (
            <p className="chart-nota">
              Mostrando os {runs.length} acionamentos mais recentes de {dados.runs_total}.
            </p>
          ) : null}
          <p className="chart-nota">
            Toque numa linha para ver o acionamento no gráfico. A variação é a temperatura ao
            desligar menos a de ligar; vale para os acionamentos automáticos. “≈” marca horários
            aproximados: o aquário ficou sem contato durante o acionamento.
          </p>
        </>
      )}
    </section>
  );
}

function Linha({
  run: r,
  agora,
  online,
  celsiusAgora,
  selecionada,
  onSelecionar,
}: {
  run: FanRun;
  agora: number;
  online: boolean;
  celsiusAgora: number | null;
  selecionada: boolean;
  onSelecionar: () => void;
}) {
  const ini = new Date(r.started_at);
  const fim = r.ended_at ? new Date(r.ended_at) : null;
  const emCurso = fim === null;
  // Em curso e com o aquário falando: conta ao vivo; senão, o que o servidor sabe.
  const duracaoS = emCurso && online ? Math.max(0, (agora - ini.getTime()) / 1000) : r.duration_s;
  const fimEmOutroDia = fim !== null && fim.toDateString() !== ini.toDateString();

  // Rodada em curso: a temperatura de saída ainda não existe; a de agora mostra o andamento.
  const aoVivo = emCurso && online && celsiusAgora !== null;
  const tempFim = aoVivo ? celsiusAgora : r.temp_end;
  const delta = r.temp_start !== null && tempFim !== null ? tempFim - r.temp_start : null;
  const pico =
    r.temp_peak !== null && r.temp_start !== null && r.temp_peak - r.temp_start >= 0.2
      ? r.temp_peak
      : null;

  return (
    <tr
      className={`acionamento ${selecionada ? "sel" : ""}`}
      tabIndex={0}
      aria-selected={selecionada}
      onClick={onSelecionar}
      onKeyDown={(e) => {
        if (e.key === "Enter" || e.key === " ") {
          e.preventDefault();
          onSelecionar();
        }
      }}
    >
      <td className="col-hora">
        {formatHora(ini)}
        {r.manual ? <span className="chip chip-manual">manual</span> : null}
        {!r.manual && r.mode === "failsafe" ? <span className="chip chip-manual">segurança</span> : null}
      </td>
      <td className="col-hora">
        {emCurso ? (
          <span className="chip chip-fan">em curso</span>
        ) : (
          <>
            {formatHora(fim)}
            {fimEmOutroDia ? <small className="mais-um" title="No dia seguinte"> +1</small> : null}
          </>
        )}
      </td>
      <td className="col-tempo">
        {formatDurationS(duracaoS)}
        {r.approx ? (
          <span className="aprox" title="O aquário ficou sem contato durante este acionamento: horários aproximados." aria-label="horários aproximados">
            {" "}
            ≈
          </span>
        ) : null}
      </td>
      <td className="col-temp">
        <div className="temp-par">
          {r.temp_start !== null ? formatTemp(r.temp_start) : "—"}
          <span className="seta" aria-hidden>
            {" → "}
          </span>
          <span className="so-leitor"> até </span>
          {tempFim !== null ? formatTemp(tempFim) : "—"}
          {aoVivo ? <small className="ao-vivo"> agora</small> : null}
        </div>
        <div className="temp-sub">
          {delta === null ? null : delta <= -DELTA_MINIMO ? (
            <span className="chip chip-bom" aria-label={`esfriou ${formatTemp(-delta)} graus`}>
              ▼ {formatTemp(-delta)}
            </span>
          ) : delta >= DELTA_MINIMO ? (
            <span className="chip chip-atencao" aria-label={`esquentou ${formatTemp(delta)} graus`}>
              ▲ {formatTemp(delta)}
            </span>
          ) : (
            <span className="chip chip-neutro">≈ estável</span>
          )}
          {pico !== null ? <span className="pico">pico {formatTemp(pico)}</span> : null}
        </div>
      </td>
    </tr>
  );
}

/**
 * Sem nenhum acionamento, a pergunta natural é "por quê?". Quando a água nem
 * chegou ao limite de ligar, é essa a resposta — e é uma boa notícia.
 */
function mensagemSemRodadas(
  dados: FanHistoryResponse,
  limites: { liga: number; desliga: number } | null,
): string {
  const base = "Nenhum acionamento neste período.";
  const t = dados.temperature;
  if (t === null || limites === null) return base;
  if (t.max < limites.liga) {
    return `${base} A água ficou entre ${formatTemp(t.min)} e ${formatTemp(t.max)} °C, sempre abaixo dos ${formatTemp(limites.liga)} °C em que a ventoinha liga.`;
  }
  return `${base} A água chegou a ${formatTemp(t.max)} °C, acima dos ${formatTemp(limites.liga)} °C de ligar — se a ventoinha estava em modo manual desligada, vale conferir.`;
}
