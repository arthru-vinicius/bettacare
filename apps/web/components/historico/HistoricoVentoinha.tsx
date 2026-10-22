"use client";

import type { DeviceConfig, FanHistoryResponse, HistoryRange } from "@bettacare/contract";
import { useMemo, useState } from "react";

import { useFanHistory } from "@/lib/useFanHistory";
import { CartaoAcionamentos } from "./CartaoAcionamentos";
import { CartaoTemperatura } from "./CartaoTemperatura";

const PERIODOS: ReadonlyArray<{ id: HistoryRange; rotulo: string }> = [
  { id: "24h", rotulo: "24 h" },
  { id: "7d", rotulo: "7 dias" },
  { id: "30d", rotulo: "30 dias" },
];

/**
 * O histórico da aba Ventoinha: a temperatura da água e os acionamentos da
 * ventoinha, no mesmo período.
 *
 * O seletor de período fica **uma vez, acima dos dois cartões** — ele vale para
 * os dois, e dois seletores (um por cartão) deixariam o gráfico de 24 h ao lado
 * de uma tabela de 30 dias. Trocar de período não pisca: os cartões antigos
 * esmaecem até os novos chegarem.
 */
export function HistoricoVentoinha({
  ativo,
  config,
  online,
  celsiusAgora,
}: {
  /** A aba está à vista? Fora dela nada é buscado. */
  ativo: boolean;
  config: DeviceConfig | null;
  online: boolean;
  /** Temperatura de agora, do estado corrente — completa a rodada em curso. */
  celsiusAgora: number | null;
}) {
  const [range, setRange] = useState<HistoryRange>("24h");
  const { dados, carregando, erro, recarregar } = useFanHistory(range, ativo);

  const trigger = config?.fan_trigger_c;
  const off = config?.fan_off_c;
  const limites = useMemo(
    () => (trigger !== undefined && off !== undefined ? { liga: trigger, desliga: off } : null),
    [trigger, off],
  );

  const trocando = carregando && dados !== null && dados.range !== range;

  return (
    <>
      <div className="hist-filtro">
        <h2 className="chart-title">Histórico</h2>
        <div className="segmentado pequeno" role="tablist" aria-label="Período do histórico">
          {PERIODOS.map((p) => (
            <button
              key={p.id}
              type="button"
              role="tab"
              aria-selected={range === p.id}
              className={range === p.id ? "ativo" : ""}
              onClick={() => setRange(p.id)}
            >
              {p.rotulo}
            </button>
          ))}
        </div>
      </div>

      {dados === null ? (
        erro ? (
          <div className="chart-card hist-card">
            <div className="empty">
              Não foi possível carregar o histórico.
              <div>
                <button type="button" className="load-more" onClick={recarregar}>
                  Tentar de novo
                </button>
              </div>
            </div>
          </div>
        ) : (
          <div className="empty">Carregando…</div>
        )
      ) : (
        <div className={trocando ? "stale" : ""} aria-busy={trocando}>
          {erro ? (
            <div className="health-hint">
              Não foi possível atualizar agora; o que está na tela é a última carga.{" "}
              <button type="button" className="hist-limpar" onClick={recarregar}>
                Tentar de novo
              </button>
            </div>
          ) : null}
          {/* `key` = período: dados novos começam sem cursor, sem seleção e na primeira página */}
          <Cartoes key={dados.range} dados={dados} limites={limites} online={online} celsiusAgora={celsiusAgora} />
        </div>
      )}
    </>
  );
}

/**
 * Os dois cartões e o que eles combinam entre si: o instante fixado no gráfico
 * (segundos) e a rodada destacada na tabela. Tocar numa rodada leva o cursor
 * ao começo dela.
 */
function Cartoes({
  dados,
  limites,
  online,
  celsiusAgora,
}: {
  dados: FanHistoryResponse;
  limites: { liga: number; desliga: number } | null;
  online: boolean;
  celsiusAgora: number | null;
}) {
  const [fixo, setFixo] = useState<number | null>(null);
  const [selecionada, setSelecionada] = useState<number | null>(null);

  const selecionar = (id: number | null) => {
    setSelecionada(id);
    if (id === null) return;
    const r = dados.runs.find((x) => x.id === id);
    // O cursor vai para o começo do acionamento (ou o começo da janela, se ele
    // começou antes dela).
    if (r !== undefined) setFixo(Math.max(Date.parse(r.started_at), Date.parse(dados.from)) / 1000);
  };

  return (
    <>
      <CartaoTemperatura
        dados={dados}
        limites={limites}
        fixo={fixo}
        onFixo={setFixo}
        selecionada={selecionada}
      />
      <CartaoAcionamentos
        dados={dados}
        limites={limites}
        online={online}
        celsiusAgora={celsiusAgora}
        selecionada={selecionada}
        onSelecionar={selecionar}
      />
    </>
  );
}
