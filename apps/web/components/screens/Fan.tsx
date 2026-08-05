"use client";

import type { CommandAction, OverviewResponse } from "@bettacare/contract";
import { useEffect, useState } from "react";

import { CommandStatus } from "@/components/CommandStatus";
import { IconFan } from "@/components/icons";
import { formatTemp } from "@/lib/format";
import type { CommandFeedback } from "@/lib/useDevice";

const MODO: Record<string, string> = {
  auto: "Controle automático por temperatura",
  manual: "Velocidade fixada manualmente",
  manual_off: "Desligada manualmente",
  failsafe: "Modo de segurança — temperatura indisponível",
};

export function Fan({
  data,
  feedback,
  onSend,
}: {
  data: OverviewResponse | null;
  feedback: CommandFeedback | null;
  onSend: (a: CommandAction) => void;
}) {
  const s = data?.state ?? null;
  const ligada = s?.fan.on ?? false;
  const online = data?.device.online ?? false;
  // Só o feedback deste alvo: um erro da outra tela não é assunto aqui.
  const meuFeedback = feedback?.target === "fan" ? feedback : null;
  const enviando = meuFeedback?.status === "pending";

  const saudeFan = data?.components.find((c) => c.comp === "fan");
  const travada = saudeFan?.status === "fault";

  /*
   * O slider é controlado localmente enquanto o dedo está nele, e só volta a
   * seguir o servidor quando solto. Sem isso, o polling de 4 s puxaria o
   * controle de volta no meio do arrasto.
   */
  const [arrastando, setArrastando] = useState(false);
  const [valor, setValor] = useState(0);

  useEffect(() => {
    if (!arrastando && s) setValor(s.fan.speed_percent);
  }, [s, arrastando]);

  return (
    <>
      <div className="screen-title">Ventoinha &amp; temperatura</div>

      <div className="card" style={{ marginBottom: 12 }}>
        <div className="c-label">Temperatura</div>
        <div className="c-val">
          {formatTemp(s?.temperature.celsius)}
          {s?.temperature.celsius !== null && s?.temperature.celsius !== undefined
            ? " °C"
            : ""}
        </div>
        {data?.config ? (
          <div className="c-sub">
            Liga em {data.config.fan_trigger_c.toFixed(1)} °C · desliga em{" "}
            {data.config.fan_off_c.toFixed(1)} °C
          </div>
        ) : null}
      </div>

      <div className={`state-hero ${ligada ? "fan-on" : ""}`}>
        <div className="hero-icon">
          <IconFan />
        </div>
        <div className="hero-state">{ligada ? "LIGADA" : "DESLIGADA"}</div>
        <div className="hero-sub">
          {s ? (MODO[s.fan.mode] ?? s.fan.mode) : "Aguardando dados"}
        </div>
      </div>

      {/*
        O tacômetro é a única realimentação real do sistema inteiro. PWM acima
        de zero com rotação zerada significa fisicamente uma coisa só.
      */}
      {travada ? (
        <div className="health-hint">
          Há sinal de PWM mas o tacômetro lê zero — a ventoinha não está
          girando. Cabo solto, rolamento travado ou fonte caída.
        </div>
      ) : null}

      <div className="card" style={{ marginBottom: 12 }}>
        <div className="c-label">Rotação</div>
        <div className="c-val">{s ? `${s.fan.rpm} rpm` : "—"}</div>
      </div>

      <div className="slider-card">
        <div className="slider-header">
          <span className="slider-lbl">Potência</span>
          <span className="slider-pct">{valor}%</span>
        </div>
        <input
          type="range"
          min={0}
          max={100}
          step={5}
          value={valor}
          disabled={!online || enviando}
          onChange={(e) => {
            setArrastando(true);
            setValor(Number(e.target.value));
          }}
          onPointerUp={() => {
            setArrastando(false);
            onSend({ action: "fan.set_speed", percent: valor });
          }}
          onKeyUp={() => {
            setArrastando(false);
            onSend({ action: "fan.set_speed", percent: valor });
          }}
        />
      </div>

      <button
        className="btn btn-fan"
        disabled={!online || enviando}
        onClick={() =>
          onSend(
            ligada
              ? { action: "fan.set_speed", percent: 0 }
              : { action: "fan.set_mode", mode: "auto" },
          )
        }
      >
        <IconFan />
        {ligada ? "Desligar ventoinha" : "Voltar ao automático"}
      </button>

      <CommandStatus feedback={meuFeedback} />
    </>
  );
}
