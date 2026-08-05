"use client";

import {
  COMPONENT_LABELS,
  describeEventCode,
  type Component,
  type EventItem,
} from "@bettacare/contract";
import { useCallback, useEffect, useRef, useState } from "react";

import { fetchEvents } from "@/lib/api";
import { formatDateTime } from "@/lib/format";

/**
 * Os logs, com filtro por tipo de entrada e por equipamento.
 *
 * Os filtros são o ponto: sem eles isto seria o mesmo despejo de texto do app
 * antigo. Com eles, "o que deu errado hoje" é um toque em *Erros*, e "o que
 * anda acontecendo com a ventoinha" é um toque no seletor.
 */

type Preset = "tudo" | "problemas" | "erros";

const PRESETS: Record<Preset, { label: string; sev?: string[] }> = {
  tudo: { label: "Tudo" },
  problemas: { label: "Avisos e erros", sev: ["warn", "error", "fatal"] },
  erros: { label: "Só erros", sev: ["error", "fatal"] },
};

const COMPONENTES_FILTRAVEIS: Component[] = [
  "temp",
  "fan",
  "light",
  "rtc",
  "wifi",
  "api",
  "button",
  "pot",
  "system",
];

export function Logs() {
  const [preset, setPreset] = useState<Preset>("tudo");
  const [comp, setComp] = useState<string>("");
  const [origem, setOrigem] = useState<string>("");

  const [items, setItems] = useState<EventItem[]>([]);
  const [cursor, setCursor] = useState<string | null>(null);
  const [carregando, setCarregando] = useState(true);
  const [erro, setErro] = useState<string | null>(null);

  // Descarta respostas de filtros já trocados: sem isso, uma resposta lenta do
  // filtro anterior sobrescreveria a do filtro atual.
  const requisicao = useRef(0);

  const carregar = useCallback(
    async (proximoCursor: string | null) => {
      const meu = ++requisicao.current;
      setCarregando(true);
      try {
        const r = await fetchEvents({
          sev: PRESETS[preset].sev,
          comp: comp ? [comp] : undefined,
          source: origem === "device" || origem === "server" ? origem : undefined,
          cursor: proximoCursor,
          limit: 40,
        });
        if (meu !== requisicao.current) return;
        setItems((antes) => (proximoCursor === null ? r.items : [...antes, ...r.items]));
        setCursor(r.next_cursor);
        setErro(null);
      } catch (e) {
        if (meu !== requisicao.current) return;
        setErro(e instanceof Error ? e.message : "falha ao carregar");
      } finally {
        if (meu === requisicao.current) setCarregando(false);
      }
    },
    [preset, comp, origem],
  );

  useEffect(() => {
    setItems([]);
    setCursor(null);
    void carregar(null);
  }, [carregar]);

  return (
    <>
      <div className="chips">
        {(Object.keys(PRESETS) as Preset[]).map((p) => (
          <button
            key={p}
            className={`chip ${p === "erros" ? "danger" : ""} ${preset === p ? "active" : ""}`}
            onClick={() => setPreset(p)}
          >
            {PRESETS[p].label}
          </button>
        ))}
      </div>

      <div className="select-row">
        <select value={comp} onChange={(e) => setComp(e.target.value)}>
          <option value="">Todos os equipamentos</option>
          {COMPONENTES_FILTRAVEIS.map((c) => (
            <option key={c} value={c}>
              {COMPONENT_LABELS[c]}
            </option>
          ))}
        </select>
        <select value={origem} onChange={(e) => setOrigem(e.target.value)}>
          <option value="">Toda origem</option>
          <option value="device">Do aquário</option>
          <option value="server">Do servidor</option>
        </select>
      </div>

      {erro ? <div className="empty">Não foi possível carregar: {erro}</div> : null}

      {items.map((e) => (
        <LogEntry key={e.id} item={e} />
      ))}

      {!carregando && items.length === 0 && erro === null ? (
        <div className="empty">Nenhum registro com esses filtros.</div>
      ) : null}

      {carregando ? <div className="empty">Carregando…</div> : null}

      {cursor !== null && !carregando ? (
        <button className="load-more" onClick={() => void carregar(cursor)}>
          Carregar mais
        </button>
      ) : null}
    </>
  );
}

function LogEntry({ item }: { item: EventItem }) {
  const info = describeEventCode(item.code);
  const problema = item.sev === "error" || item.sev === "fatal" || item.sev === "warn";

  /*
   * A hora do RTC do aquário, quando existe, vale mais que o carimbo do
   * servidor: eventos represados por falta de rede chegam todos com o mesmo
   * `received_at`, e é o `device_time` que diz quando cada um aconteceu.
   */
  const horaDispositivo =
    item.ctx !== null && typeof item.ctx["device_time"] === "string"
      ? (item.ctx["device_time"] as string)
      : null;

  return (
    <div className={`log-entry ${item.sev}`}>
      <div className="log-head">
        <span className="log-ts">
          {horaDispositivo ?? formatDateTime(item.received_at)}
        </span>
        <span className="log-comp">{COMPONENT_LABELS[item.comp]}</span>
        <span className="log-source">
          {item.source === "device" ? "aquário" : "servidor"}
        </span>
      </div>

      <div className="log-label">
        {info?.label ?? item.code}
        {item.repeat_count > 1 ? (
          <span className="log-repeat">×{item.repeat_count}</span>
        ) : null}
      </div>

      <div className="log-msg">{item.msg}</div>

      {problema && info?.hint ? <div className="log-hint">{info.hint}</div> : null}
    </div>
  );
}
