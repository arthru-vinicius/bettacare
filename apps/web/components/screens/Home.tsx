"use client";

import type { OverviewResponse } from "@bettacare/contract";
import { useEffect, useState } from "react";

import { formatInt, formatTemp, timeAgo } from "@/lib/format";

/**
 * Tela inicial — o resumo, e o atalho para cada coisa.
 *
 * Tudo que tem tela própria aqui é tocável e leva até ela: a temperatura vai
 * para a ventoinha (onde ficam os limites e o histórico), e os dois cartões
 * para as telas deles. Quem abre o app quer saber "está tudo bem?" e, se
 * não estiver, chegar no lugar certo com um toque.
 */
export function Home({
  data,
  deviceMissing,
  onOpenFan,
  onOpenLight,
}: {
  data: OverviewResponse | null;
  deviceMissing: boolean;
  onOpenFan: () => void;
  onOpenLight: () => void;
}) {
  const relogio = useLocalClock();
  const s = data?.state ?? null;
  const online = data?.device.online ?? false;

  return (
    <>
      <button className="temp-hero" onClick={onOpenFan}>
        <div className="temp-hero-lbl">Temperatura da água</div>
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
            : "Toque para ver o histórico e os limites"}
        </div>
      </button>

      <div className="grid-2">
        <button className="card card-toque" onClick={onOpenLight}>
          <div className="c-label">Luminária</div>
          <span className={`pill ${s?.light.on ? "amber" : "off"}`}>
            <span className="pill-dot" />
            {s?.light.on ? "ACESA" : "APAGADA"}
          </span>
          {data?.config ? (
            <div className="c-sub">
              {data.config.light_on_time}–{data.config.light_off_time}
            </div>
          ) : null}
        </button>

        <button className="card card-toque" onClick={onOpenFan}>
          <div className="c-label">Ventoinha</div>
          <span className={`pill ${s?.fan.on ? "on" : "off"}`}>
            <span className="pill-dot" />
            {s?.fan.on ? "LIGADA" : "DESLIGADA"}
          </span>
          {s ? (
            <div className="c-sub">
              {s.fan.on
                ? `${s.fan.speed_percent}% · ${formatInt(s.fan.rpm)} rpm`
                : s.fan.mode === "auto" || s.fan.mode === "failsafe"
                  ? "no automático"
                  : "no manual"}
            </div>
          ) : null}
        </button>
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
