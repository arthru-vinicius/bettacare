"use client";

import {
  COMPONENT_LABELS,
  describeEventCode,
  type Component,
  type EventItem,
  type Severity,
} from "@bettacare/contract";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";

import { IconChevron, IconDownload } from "@/components/icons";
import { exportUrl, fetchEvents, type EventQuery } from "@/lib/api";
import { dayKey, formatClock, formatDateTime, formatDayHeader } from "@/lib/format";

/**
 * Registros — o que eram as abas Logs e Relatórios, numa tela só.
 *
 * Uma pergunta, um lugar: "o que aconteceu no aquário nesse período?". A
 * lista responde olhando; a planilha responde guardando ou cruzando no Excel.
 * As duas usam exatamente os mesmos filtros, então o que se baixa é o que se
 * vê.
 *
 * Os filtros mais usados (período e nível) ficam sempre à mostra; os de
 * investigação (equipamento, origem, busca) ficam recolhidos — a maioria das
 * visitas só quer saber "deu algo errado hoje?".
 */

type Periodo = "hoje" | "7d" | "30d" | "livre";
type Nivel = "tudo" | "problemas" | "erros";

const PERIODOS: Record<Periodo, string> = {
  hoje: "Hoje",
  "7d": "7 dias",
  "30d": "30 dias",
  livre: "Datas",
};

const NIVEIS: Record<Nivel, { label: string; sev?: Severity[] }> = {
  tudo: { label: "Tudo" },
  problemas: { label: "Avisos e erros", sev: ["warn", "error", "fatal"] },
  erros: { label: "Só erros", sev: ["error", "fatal"] },
};

const NIVEL_ROTULO: Record<Severity, string> = {
  debug: "Depuração",
  info: "Info",
  warn: "Aviso",
  error: "Erro",
  fatal: "Crítico",
};

const COMPONENTES_FILTRAVEIS: Component[] = [
  "temp",
  "fan",
  "light",
  "rtc",
  "button",
  "pot",
  "feeder",
  "wifi",
  "api",
  "nvs",
  "ota",
  "system",
];

export function Logs() {
  const [periodo, setPeriodo] = useState<Periodo>("hoje");
  const [de, setDe] = useState(() => isoDia(diasAtras(6)));
  const [ate, setAte] = useState(() => isoDia(new Date()));
  const [nivel, setNivel] = useState<Nivel>("tudo");
  const [comp, setComp] = useState("");
  const [origem, setOrigem] = useState("");
  const [busca, setBusca] = useState("");
  /** Debounced: sem isto, cada tecla digitada dispararia uma requisição nova (UPGRADE/06, P2). */
  const [buscaEfetiva, setBuscaEfetiva] = useState("");
  const [maisFiltros, setMaisFiltros] = useState(false);

  const [items, setItems] = useState<EventItem[]>([]);
  const [cursor, setCursor] = useState<string | null>(null);
  const [carregando, setCarregando] = useState(true);
  const [erro, setErro] = useState<string | null>(null);
  const [aberto, setAberto] = useState<string | null>(null);

  // Descarta respostas de filtros já trocados: sem isso, uma resposta lenta do
  // filtro anterior sobrescreveria a do filtro atual.
  const requisicao = useRef(0);

  useEffect(() => {
    const t = setTimeout(() => setBuscaEfetiva(busca), 300);
    return () => clearTimeout(t);
  }, [busca]);

  const consulta: EventQuery = useMemo(() => {
    const { from, to } = intervaloDe(periodo, de, ate);
    return {
      sev: NIVEIS[nivel].sev,
      comp: comp ? [comp] : undefined,
      source: origem === "device" || origem === "server" ? origem : undefined,
      q: buscaEfetiva || undefined,
      from,
      to,
    };
  }, [periodo, de, ate, nivel, comp, origem, buscaEfetiva]);

  const carregar = useCallback(
    async (proximoCursor: string | null) => {
      const meu = ++requisicao.current;
      setCarregando(true);
      try {
        const r = await fetchEvents({ ...consulta, cursor: proximoCursor, limit: 40 });
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
    [consulta],
  );

  useEffect(() => {
    setItems([]);
    setCursor(null);
    setAberto(null);
    void carregar(null);
  }, [carregar]);

  const filtrosExtras = [comp, origem, buscaEfetiva].filter(Boolean).length;
  const datasInvalidas = periodo === "livre" && (!de || !ate || de > ate);

  const grupos = useMemo(() => {
    const porDia: { chave: string; titulo: string; itens: EventItem[] }[] = [];
    for (const e of items) {
      const chave = dayKey(e.received_at);
      const ultimo = porDia.at(-1);
      if (ultimo && ultimo.chave === chave) ultimo.itens.push(e);
      else porDia.push({ chave, titulo: formatDayHeader(e.received_at), itens: [e] });
    }
    return porDia;
  }, [items]);

  return (
    <>
      <div className="filtros">
        <div className="segmentado" role="tablist" aria-label="Período">
          {(Object.keys(PERIODOS) as Periodo[]).map((p) => (
            <button
              key={p}
              role="tab"
              aria-selected={periodo === p}
              className={periodo === p ? "ativo" : ""}
              onClick={() => setPeriodo(p)}
            >
              {PERIODOS[p]}
            </button>
          ))}
        </div>

        {periodo === "livre" ? (
          <div className="campo-par">
            <label className="campo">
              <span className="campo-rotulo">De</span>
              <input type="date" value={de} max={ate} onChange={(e) => setDe(e.target.value)} />
            </label>
            <label className="campo">
              <span className="campo-rotulo">Até</span>
              <input
                type="date"
                value={ate}
                min={de}
                max={isoDia(new Date())}
                onChange={(e) => setAte(e.target.value)}
              />
            </label>
          </div>
        ) : null}

        <div className="segmentado" role="tablist" aria-label="Nível">
          {(Object.keys(NIVEIS) as Nivel[]).map((n) => (
            <button
              key={n}
              role="tab"
              aria-selected={nivel === n}
              className={`${nivel === n ? "ativo" : ""} ${n === "erros" ? "perigo" : ""}`}
              onClick={() => setNivel(n)}
            >
              {NIVEIS[n].label}
            </button>
          ))}
        </div>

        <button
          className={`filtros-mais ${maisFiltros ? "aberto" : ""}`}
          onClick={() => setMaisFiltros((v) => !v)}
          aria-expanded={maisFiltros}
        >
          Mais filtros{filtrosExtras > 0 ? ` · ${filtrosExtras} ativo${filtrosExtras > 1 ? "s" : ""}` : ""}
          <IconChevron />
        </button>

        {maisFiltros ? (
          <div className="filtros-extras">
            <input
              className="busca"
              type="search"
              value={busca}
              onChange={(e) => setBusca(e.target.value)}
              placeholder="Buscar no texto ou no código…"
              aria-label="Buscar nos registros"
            />
            <div className="select-row">
              <select value={comp} onChange={(e) => setComp(e.target.value)} aria-label="Equipamento">
                <option value="">Todo equipamento</option>
                {COMPONENTES_FILTRAVEIS.map((c) => (
                  <option key={c} value={c}>
                    {COMPONENT_LABELS[c]}
                  </option>
                ))}
              </select>
              <select value={origem} onChange={(e) => setOrigem(e.target.value)} aria-label="Origem">
                <option value="">Toda origem</option>
                <option value="device">Do aquário</option>
                <option value="server">Do servidor</option>
              </select>
            </div>
          </div>
        ) : null}
      </div>

      <div className="exportar">
        <div className="exportar-texto">
          <strong>Baixar planilha</strong>
          <span>Com o período e os filtros acima. Abre no Excel e no Google Planilhas.</span>
        </div>
        <div className="exportar-botoes">
          <a
            className={`btn-exportar ${datasInvalidas ? "desabilitado" : ""}`}
            href={datasInvalidas ? undefined : exportUrl("events", consulta)}
            download
            aria-disabled={datasInvalidas}
          >
            <IconDownload />
            Registros
          </a>
          <a
            className={`btn-exportar ${datasInvalidas ? "desabilitado" : ""}`}
            href={datasInvalidas ? undefined : exportUrl("telemetry", consulta)}
            download
            aria-disabled={datasInvalidas}
          >
            <IconDownload />
            Medições
          </a>
        </div>
      </div>

      {datasInvalidas ? (
        <div className="empty">A data inicial precisa vir antes da final.</div>
      ) : null}
      {erro ? <div className="empty">Não foi possível carregar: {erro}</div> : null}

      {grupos.map((g) => (
        <section key={g.chave} className="log-dia">
          <h3 className="log-dia-titulo">{g.titulo}</h3>
          {g.itens.map((e) => (
            <LogEntry
              key={e.id}
              item={e}
              aberto={aberto === e.id}
              onToggle={() => setAberto((a) => (a === e.id ? null : e.id))}
            />
          ))}
        </section>
      ))}

      {!carregando && items.length === 0 && erro === null && !datasInvalidas ? (
        <div className="empty">
          {nivel === "tudo" ? "Nenhum registro neste período." : "Nada de errado neste período. 👍"}
        </div>
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

function LogEntry({
  item,
  aberto,
  onToggle,
}: {
  item: EventItem;
  aberto: boolean;
  onToggle: () => void;
}) {
  const info = describeEventCode(item.code);
  const problema = item.sev === "error" || item.sev === "fatal" || item.sev === "warn";

  /*
   * Evento que ficou represado no aquário (sem rede) chega com o carimbo do
   * servidor atrasado. A hora do relógio do aquário diz quando ele de fato
   * aconteceu — só vale mostrar quando diverge.
   */
  const horaAquario =
    item.ctx !== null && typeof item.ctx["device_time"] === "string"
      ? (item.ctx["device_time"] as string)
      : null;
  const contexto = item.ctx
    ? Object.entries(item.ctx).filter(([k]) => k !== "device_time")
    : [];

  // O botão é só o cabeçalho: os detalhes ficam fora dele para dar para
  // selecionar e copiar o código e o contexto.
  return (
    <div className={`log-item ${item.sev}`}>
      <button className="log-cabeca" onClick={onToggle} aria-expanded={aberto}>
        <span className="log-linha1">
          <span className="log-hora">{formatClock(item.received_at)}</span>
          <span className={`log-nivel ${item.sev}`}>{NIVEL_ROTULO[item.sev]}</span>
          <span className="log-equip">{COMPONENT_LABELS[item.comp]}</span>
          {item.repeat_count > 1 ? <span className="log-repeat">×{item.repeat_count}</span> : null}
        </span>
        <span className="log-titulo">{info?.label ?? item.code}</span>
        <span className="log-msg">{item.msg}</span>
        {problema && info?.hint ? <span className="log-hint">{info.hint}</span> : null}
      </button>

      {aberto ? (
        <dl className="log-detalhes">
          <dt>Origem</dt>
          <dd>{item.source === "device" ? "Aquário" : "Servidor"}</dd>
          <dt>Recebido</dt>
          <dd>{formatDateTime(item.received_at)}</dd>
          {horaAquario ? (
            <>
              <dt>No relógio do aquário</dt>
              <dd>{horaAquario}</dd>
            </>
          ) : null}
          <dt>Código</dt>
          <dd className="mono">{item.code}</dd>
          {contexto.length > 0 ? (
            <>
              <dt>Detalhes</dt>
              <dd className="mono">
                {contexto.map(([k, v]) => (
                  <div key={k}>
                    {k}: {typeof v === "string" ? v : JSON.stringify(v)}
                  </div>
                ))}
              </dd>
            </>
          ) : null}
        </dl>
      ) : null}
    </div>
  );
}

// ── Período ──────────────────────────────────────────────────────────────────

function diasAtras(n: number): Date {
  const d = new Date();
  return new Date(d.getFullYear(), d.getMonth(), d.getDate() - n);
}

/** `2026-10-02` no fuso do navegador — o formato do `<input type="date">`. */
function isoDia(d: Date): string {
  const p = (n: number) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`;
}

/** Meia-noite local do dia `YYYY-MM-DD`. */
function meiaNoite(dia: string): Date {
  const [a, m, d] = dia.split("-").map(Number) as [number, number, number];
  return new Date(a, m - 1, d);
}

/**
 * Dias inteiros no fuso de quem está olhando: "hoje" é desde a meia-noite
 * local, e o fim é a meia-noite seguinte (exclusivo) — o mesmo recorte que a
 * planilha usa.
 */
function intervaloDe(p: Periodo, de: string, ate: string): { from?: Date; to?: Date } {
  const amanha = diasAtras(-1);
  switch (p) {
    case "hoje":
      return { from: diasAtras(0), to: amanha };
    case "7d":
      return { from: diasAtras(6), to: amanha };
    case "30d":
      return { from: diasAtras(29), to: amanha };
    case "livre": {
      if (!de || !ate || de > ate) return {};
      const fim = meiaNoite(ate);
      return { from: meiaNoite(de), to: new Date(fim.getFullYear(), fim.getMonth(), fim.getDate() + 1) };
    }
  }
}
