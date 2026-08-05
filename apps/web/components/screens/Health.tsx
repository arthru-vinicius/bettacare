"use client";

import {
  COMPONENT_LABELS,
  describeEventCode,
  HEALTH_STATUS_LABELS,
  type ComponentHealth,
  type OverviewResponse,
} from "@bettacare/contract";

import { formatTime, timeAgo } from "@/lib/format";

/**
 * Saúde componente a componente.
 *
 * A ordem é fixa e coloca o hardware do aquário primeiro: é o que o usuário
 * quer ver. Infraestrutura fica embaixo porque só importa quando quebra — e
 * quando quebra, a cor a traz para a atenção sozinha.
 */
const ORDEM = ["temp", "fan", "light", "rtc", "wifi", "api", "system"] as const;

export function Health({ data }: { data: OverviewResponse | null }) {
  if (data === null) return <div className="empty">Carregando…</div>;

  const online = data.device.online;
  const ordenados = [...data.components].sort(
    (a, b) => indice(a.comp) - indice(b.comp),
  );

  return (
    <>
      <div className="card" style={{ marginBottom: 12 }}>
        <div className="row-between">
          <div>
            <div className="health-name">{data.device.name}</div>
            <div className="health-detail">
              {data.device.fw_version ? `Firmware ${data.device.fw_version}` : "—"}
              {data.device.last_ip ? ` · ${data.device.last_ip}` : ""}
            </div>
          </div>
          <span className={`pill ${online ? "on" : "danger"}`}>
            <span className="pill-dot" />
            {online ? "ONLINE" : "OFFLINE"}
          </span>
        </div>
        <div className="c-sub">
          {online
            ? `Última telemetria ${timeAgo(data.state?.updated_at)}`
            : `Sem contato ${timeAgo(data.device.last_seen_at)}`}
        </div>
      </div>

      {!online ? (
        <div className="health-hint">
          Com o dispositivo mudo, o estado de cada componente passa a ser
          desconhecido. Manter o último valor como se ainda valesse seria
          afirmar algo que não se sabe mais.
        </div>
      ) : null}

      {ordenados.map((c) => (
        <ComponentRow key={c.comp} health={c} />
      ))}

      {ordenados.length === 0 ? (
        <div className="empty">
          Nenhuma informação de saúde ainda. Ela aparece no primeiro contato do
          dispositivo.
        </div>
      ) : null}
    </>
  );
}

function ComponentRow({ health }: { health: ComponentHealth }) {
  const info = health.last_code ? describeEventCode(health.last_code) : undefined;
  const problema = health.status !== "ok" && health.status !== "unknown";
  const valor = valorDe(health);

  /*
   * Com o dispositivo offline o status vira `unknown`, mas o `detail` guarda o
   * último valor recebido. Mostrá-lo ao lado de "Sem informação" seria
   * contraditório: o número precisa vir marcado como último conhecido, e
   * apagado, para não ser lido como leitura atual.
   */
  const desatualizado = health.status === "unknown" && valor !== "";

  return (
    <>
      <div className="health-row">
        <span className={`health-dot ${health.status}`} />
        <div className="health-main">
          <div className="health-name">{COMPONENT_LABELS[health.comp]}</div>
          <div className="health-detail">
            {desatualizado
              ? `Último valor conhecido · ${formatTime(health.last_ok_at ?? health.since)}`
              : HEALTH_STATUS_LABELS[health.status]}
            {problema ? ` · desde ${formatTime(health.since)}` : ""}
            {health.status === "ok" && health.last_ok_at
              ? ` · ${timeAgo(health.last_ok_at)}`
              : ""}
          </div>
        </div>
        <div className={`health-value ${desatualizado ? "stale" : ""}`}>{valor}</div>
      </div>
      {problema && info?.hint ? <div className="health-hint">{info.hint}</div> : null}
    </>
  );
}

/** O número que faz sentido para cada componente. */
function valorDe(h: ComponentHealth): string {
  const d = h.detail;
  if (d === null || d === undefined) return "";

  switch (h.comp) {
    case "temp": {
      const c = d["celsius"];
      return typeof c === "number" ? `${c.toFixed(1)} °C` : "—";
    }
    case "fan": {
      const rpm = d["rpm"];
      const pct = d["speed_percent"];
      if (typeof rpm !== "number") return "—";
      return typeof pct === "number" && pct > 0 ? `${rpm} rpm` : "parada";
    }
    case "light": {
      const on = d["on"];
      return typeof on === "boolean" ? (on ? "acesa" : "apagada") : "—";
    }
    case "rtc": {
      const t = d["time"];
      return typeof t === "string" ? t : "—";
    }
    case "wifi": {
      const r = d["rssi"];
      return typeof r === "number" ? `${r} dBm` : "—";
    }
    default:
      return "";
  }
}

function indice(comp: string): number {
  const i = (ORDEM as readonly string[]).indexOf(comp);
  return i === -1 ? 99 : i;
}
