"use client";

import type { FanHistoryResponse } from "@bettacare/contract";
import { useMemo } from "react";

import { formatDia, formatHora, formatTemp } from "@/lib/format";
import { buracosDaSerie, limitesRelevantes, resumir } from "@/lib/grafico";
import { GraficoTemperatura } from "./GraficoTemperatura";

const PERIODO: Record<FanHistoryResponse["range"], string> = {
  "24h": "as últimas 24 horas",
  "7d": "os últimos 7 dias",
  "30d": "os últimos 30 dias",
};

const quando = (iso: string): string => {
  const d = new Date(iso);
  return `${formatDia(d)} ${formatHora(d)}`;
};

/**
 * O cartão da temperatura: o gráfico interativo, a legenda, os extremos do
 * período (que levam o cursor até o momento em que aconteceram) e a tabela
 * equivalente.
 */
export function CartaoTemperatura({
  dados,
  limites,
  fixo,
  onFixo,
  selecionada,
}: {
  dados: FanHistoryResponse;
  limites: { liga: number; desliga: number } | null;
  fixo: number | null;
  onFixo: (t: number | null) => void;
  selecionada: number | null;
}) {
  const t0 = Date.parse(dados.from) / 1000;
  const t1 = Date.parse(dados.to) / 1000;
  const temp = dados.temperature;

  const { temBuracos, mostraLimites } = useMemo(
    () => ({
      temBuracos: buracosDaSerie(dados.points, dados.bucket_s, dados.gap_s, t0, t1).length > 0,
      mostraLimites: temp !== null && limitesRelevantes(temp.min, temp.max, limites),
    }),
    [dados, limites, t0, t1, temp],
  );
  const linhas = useMemo(() => resumir(dados.points, dados.range).reverse(), [dados]);

  if (dados.points.length === 0) {
    return (
      <section className="chart-card hist-card" aria-label="Temperatura da água">
        <div className="chart-title">Temperatura da água</div>
        <div className="empty">
          Sem leituras de temperatura em {PERIODO[dados.range]}. Assim que o aquário
          voltar a falar com o servidor, elas aparecem aqui.
        </div>
      </section>
    );
  }

  const resolucao =
    dados.bucket_s <= 120 ? "1 min" : dados.bucket_s < 3600 ? `${dados.bucket_s / 60} min` : "1 h";

  return (
    <section className="chart-card hist-card" aria-label="Temperatura da água">
      <div className="chart-title">Temperatura da água</div>

      <GraficoTemperatura
        dados={dados}
        limites={limites}
        fixo={fixo}
        onFixo={onFixo}
        selecionada={selecionada}
      />

      <ul className="hist-legenda" aria-label="Legenda do gráfico">
        <li>
          <span className="chave chave-linha" aria-hidden />
          Temperatura
        </li>
        <li>
          <span className="chave chave-fan" aria-hidden />
          Ventoinha ligada
        </li>
        {mostraLimites ? (
          <li>
            <span className="chave chave-limite" aria-hidden />
            Limites da ventoinha
          </li>
        ) : null}
        {temBuracos ? (
          <li>
            <span className="chave chave-buraco" aria-hidden />
            Sem leitura
          </li>
        ) : null}
      </ul>

      {temp !== null ? (
        <div className="hist-stats">
          <button
            type="button"
            className="hist-stat"
            onClick={() => onFixo(Date.parse(temp.min_at) / 1000)}
            aria-label={`Mínima ${formatTemp(temp.min)} graus, ${quando(temp.min_at)}. Mostrar no gráfico.`}
          >
            <span className="stat-lbl">Mínima</span>
            <span className="stat-val">
              {formatTemp(temp.min)}
              <small> °C</small>
            </span>
            <span className="stat-quando">{quando(temp.min_at)}</span>
          </button>
          <div className="hist-stat media">
            <span className="stat-lbl">Média</span>
            <span className="stat-val">
              {formatTemp(temp.avg)}
              <small> °C</small>
            </span>
            <span className="stat-quando">no período</span>
          </div>
          <button
            type="button"
            className="hist-stat"
            onClick={() => onFixo(Date.parse(temp.max_at) / 1000)}
            aria-label={`Máxima ${formatTemp(temp.max)} graus, ${quando(temp.max_at)}. Mostrar no gráfico.`}
          >
            <span className="stat-lbl">Máxima</span>
            <span className="stat-val">
              {formatTemp(temp.max)}
              <small> °C</small>
            </span>
            <span className="stat-quando">{quando(temp.max_at)}</span>
          </button>
        </div>
      ) : null}

      <p className="chart-nota">
        Cada ponto é a média de {resolucao}
        {dados.bucket_s >= 300 ? "; a faixa clara mostra a mínima e a máxima dentro dele" : ""}. A
        linha se interrompe quando o aquário ficou sem contato.
      </p>

      <details className="hist-tabela">
        <summary>Ver como tabela</summary>
        <div className="hist-tabela-rolagem">
        <table>
          <caption className="so-leitor">
            Temperatura da água por {dados.range === "24h" ? "hora" : "dia"}, de {PERIODO[dados.range]}
          </caption>
          <thead>
            <tr>
              <th scope="col">{dados.range === "24h" ? "Hora" : "Dia"}</th>
              <th scope="col">Mín</th>
              <th scope="col">Média</th>
              <th scope="col">Máx</th>
            </tr>
          </thead>
          <tbody>
            {linhas.map((l) => {
              const d = new Date(l.t * 1000);
              return (
                <tr key={l.t}>
                  <th scope="row">
                    {dados.range === "24h"
                      ? `${formatDia(d)} ${String(d.getHours()).padStart(2, "0")}h`
                      : formatDia(d)}
                  </th>
                  <td>{formatTemp(l.min)}</td>
                  <td>{formatTemp(l.avg)}</td>
                  <td>{formatTemp(l.max)}</td>
                </tr>
              );
            })}
          </tbody>
        </table>
        </div>
      </details>
    </section>
  );
}
