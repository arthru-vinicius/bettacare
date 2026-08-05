"use client";

import type { OverviewResponse } from "@bettacare/contract";
import { useEffect, useState } from "react";

import { formatTemp, timeAgo } from "@/lib/format";

/**
 * Tela inicial.
 *
 * O card de temperatura é tocável e leva ao histórico — foi a escolha para
 * manter o app com quatro abas em vez de cinco, sem esconder os relatórios.
 */
export function Home({
  data,
  deviceMissing,
  onOpenReport,
}: {
  data: OverviewResponse | null;
  deviceMissing: boolean;
  onOpenReport: () => void;
}) {
  const relogio = useLocalClock();
  const s = data?.state ?? null;
  const online = data?.device.online ?? false;

  return (
    <>
      <button className="temp-hero" onClick={onOpenReport}>
        <div className="temp-hero-lbl">Temperatura</div>
        <div>
          <span className="temp-hero-num">
            {formatTemp(s?.temperature.celsius)}
          </span>
          {s?.temperature.celsius !== null && s?.temperature.celsius !== undefined ? (
            <span className="temp-hero-unit">°C</span>
          ) : null}
        </div>
        <div className="temp-hero-hint">
          {s?.temperature.available === false
            ? "Sensor não reconhecido"
            : "Toque para ver o histórico"}
        </div>
      </button>

      <div className="grid-2">
        <div className="card">
          <div className="c-label">Luminária</div>
          <span className={`pill ${s?.light.on ? "amber" : "off"}`}>
            <span className="pill-dot" />
            {s?.light.on ? "ACESA" : "APAGADA"}
          </span>
        </div>

        <div className="card">
          <div className="c-label">Ventoinha</div>
          <span className={`pill ${s?.fan.on ? "on" : "off"}`}>
            <span className="pill-dot" />
            {s?.fan.on ? "LIGADA" : "DESLIGADA"}
          </span>
          {s?.fan.on ? (
            <div className="c-sub">
              {s.fan.speed_percent}% · {s.fan.rpm} rpm
            </div>
          ) : null}
        </div>
      </div>

      <div className="clock-card">
        <div className="clock-lbl">Hora do sistema</div>
        <div className="clock-digits">
          <span>{relogio.h}</span>
          <span className="clock-sep">:</span>
          <span>{relogio.m}</span>
          <span className="clock-sep">:</span>
          <span>{relogio.s}</span>
        </div>
      </div>

      <div className="card">
        <div className="row-between">
          <span className="c-label" style={{ marginBottom: 0 }}>
            Controlador
          </span>
          {/*
            "Nunca se apresentou" e "sumiu" são situações diferentes, e chamar a
            primeira de OFFLINE em vermelho assusta à toa: nada quebrou, o
            aquário só ainda não foi ligado pela primeira vez.
          */}
          <span
            className={`pill ${deviceMissing ? "off" : online ? "on" : "danger"}`}
          >
            <span className="pill-dot" />
            {deviceMissing ? "AGUARDANDO" : online ? "ONLINE" : "OFFLINE"}
          </span>
        </div>
        <div className="c-sub">
          {deviceMissing
            ? "Nenhum contato ainda — grave o firmware no ESP32 para começar"
            : online
              ? `Última leitura ${timeAgo(s?.updated_at)}`
              : `Sem contato ${timeAgo(data?.device.last_seen_at)}`}
        </div>
      </div>
    </>
  );
}

/**
 * Relógio da interface.
 *
 * Roda no navegador, não vem do servidor: é o relógio do usuário. O horário do
 * RTC do aquário aparece na tela da Luminária, onde ele importa — e é lá que
 * uma divergência entre os dois vira informação de diagnóstico.
 */
function useLocalClock() {
  const [t, setT] = useState({ h: "--", m: "--", s: "--" });

  useEffect(() => {
    const tick = () => {
      const d = new Date();
      setT({
        h: String(d.getHours()).padStart(2, "0"),
        m: String(d.getMinutes()).padStart(2, "0"),
        s: String(d.getSeconds()).padStart(2, "0"),
      });
    };
    tick();
    const id = setInterval(tick, 1000);
    return () => clearInterval(id);
  }, []);

  return t;
}
