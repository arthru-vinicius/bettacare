"use client";

import type { CommandAction, OverviewResponse } from "@bettacare/contract";

import { CommandStatus } from "@/components/CommandStatus";
import { IconBulb } from "@/components/icons";
import type { CommandFeedback } from "@/lib/useDevice";

const ORIGEM: Record<string, string> = {
  schedule: "Pela automação de horário",
  manual: "Alterada manualmente",
  button: "Pelo botão físico",
  command: "Por comando do app",
  boot: "Estado inicial",
};

export function Light({
  data,
  feedback,
  onSend,
}: {
  data: OverviewResponse | null;
  feedback: CommandFeedback | null;
  onSend: (a: CommandAction) => void;
}) {
  const s = data?.state ?? null;
  const acesa = s?.light.on ?? false;
  const online = data?.device.online ?? false;
  // Só o feedback deste alvo: um erro da outra tela não é assunto aqui.
  const meuFeedback = feedback?.target === "light" ? feedback : null;
  const enviando = meuFeedback?.status === "pending";

  const saudeLuz = data?.components.find((c) => c.comp === "light");
  const emFalha = saudeLuz?.status === "fault";

  return (
    <>
      <div className="screen-title">Luminária</div>

      <div className={`state-hero ${acesa ? "light-on" : ""}`}>
        <div className="hero-icon">
          <IconBulb />
        </div>
        <div className="hero-state">{acesa ? "ACESA" : "APAGADA"}</div>
        <div className="hero-sub">
          {s ? (ORIGEM[s.light.source] ?? s.light.source) : "Aguardando dados"}
        </div>
      </div>

      {/*
        A luminária é o único atuador sem realimentação: o SSR não diz se a
        lâmpada acendeu. Quando o servidor detecta divergência entre o que foi
        comandado e o que o dispositivo reporta, é isto que o usuário precisa
        ver — e no lugar onde ele está tentando agir.
      */}
      {emFalha ? (
        <div className="health-hint">
          O dispositivo aceitou o comando, mas o estado reportado não mudou.
          Suspeite do SSR ou da alimentação da lâmpada.
        </div>
      ) : null}

      <div className="sched-row">
        <div className="sched-col">
          <div className="sched-lbl">Acende</div>
          <div className="sched-val">{data?.config?.light_on_time ?? "--:--"}</div>
        </div>
        <div className="sched-sep" />
        <div className="sched-col">
          <div className="sched-lbl">Apaga</div>
          <div className="sched-val">{data?.config?.light_off_time ?? "--:--"}</div>
        </div>
      </div>

      <div className="mini-clock-row">
        <span className="mini-clock-lbl">Relógio do aquário</span>
        <span className="mini-clock-val">
          {s?.rtc.available ? (s.rtc.time ?? "--:--") : "--:--"}
        </span>
      </div>

      <button
        className="btn btn-light"
        disabled={!online || enviando}
        onClick={() => onSend({ action: "light.set", on: !acesa })}
      >
        <IconBulb />
        {acesa ? "Apagar luminária" : "Acender luminária"}
      </button>

      <CommandStatus feedback={meuFeedback} />

      {!online && meuFeedback === null ? (
        <div className="cmd-status fail">
          <span aria-hidden>✕</span>
          <span>
            Dispositivo offline
            <span className="cmd-status-hint">
              O controle volta assim que ele retomar contato com o servidor.
            </span>
          </span>
        </div>
      ) : null}
    </>
  );
}
