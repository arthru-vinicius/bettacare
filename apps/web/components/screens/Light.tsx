"use client";

import type { CommandAction, DeviceConfig, OverviewResponse } from "@bettacare/contract";
import { useCallback, useEffect, useState } from "react";

import { CommandStatus } from "@/components/CommandStatus";
import { IconBulb, IconPencil } from "@/components/icons";
import { SettingsStatus } from "@/components/SettingsStatus";
import { LightScheduleSheet } from "@/components/sheets/LightScheduleSheet";
import type { CommandFeedback, SettingsArea, SettingsFeedback } from "@/lib/useDevice";

const ORIGEM: Record<string, string> = {
  schedule: "Pela automação de horário",
  manual: "Alterada manualmente",
  button: "Pelo botão físico",
  command: "Pelo app",
  boot: "Estado inicial, depois de ligar",
};

export function Light({
  data,
  feedback,
  onSend,
  settingsFeedback,
  onSaveSettings,
}: {
  data: OverviewResponse | null;
  feedback: CommandFeedback | null;
  onSend: (a: CommandAction) => void;
  settingsFeedback: SettingsFeedback | null;
  onSaveSettings: (patch: Partial<DeviceConfig>, area: SettingsArea) => Promise<boolean>;
}) {
  const s = data?.state ?? null;
  const online = data?.device.online ?? false;
  // Só os avisos desta tela: um erro da ventoinha não é assunto aqui.
  const meuFeedback = feedback?.target === "light" ? feedback : null;
  const meuAjuste = settingsFeedback?.area === "light" ? settingsFeedback : null;
  const enviando = meuFeedback?.status === "pending";

  /*
   * Valor otimista: mostra o estado pedido assim que o botão é clicado, sem
   * esperar o servidor confirmar. Só sai de cena quando o servidor realmente
   * reporta esse valor (telemetria alcançou o pedido) ou quando o comando
   * falha — nesse caso volta a confiar no que o servidor diz de verdade.
   */
  const [otimista, setOtimista] = useState<boolean | null>(null);
  const [editando, setEditando] = useState(false);

  useEffect(() => {
    if (otimista === null) return;
    if (meuFeedback?.status === "fail") {
      setOtimista(null);
      return;
    }
    if (s && s.light.on === otimista) setOtimista(null);
  }, [s, meuFeedback, otimista]);

  const acesa = otimista ?? s?.light.on ?? false;
  const emFalha = data?.components.find((c) => c.comp === "light")?.status === "fault";
  const fecharEditor = useCallback(() => setEditando(false), []);

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

      {emFalha ? (
        <div className="health-hint">
          O aquário diz que mudou a luz pelo comando, mas reporta outro estado.
          Vale conferir o firmware e o SSR.
        </div>
      ) : null}

      {/*
        Os dois horários abrem o mesmo editor: quem quer mudar o "acende"
        quase sempre confere o "apaga" junto, e a duração diária só faz
        sentido vendo os dois.
      */}
      <div className="sched-row sched-editavel">
        <button
          className="sched-col"
          onClick={() => setEditando(true)}
          disabled={!data?.config}
          aria-label="Ajustar o horário de acender"
        >
          <div className="sched-lbl">Acende</div>
          <div className="sched-val">{data?.config?.light_on_time ?? "--:--"}</div>
        </button>
        <div className="sched-sep" />
        <button
          className="sched-col"
          onClick={() => setEditando(true)}
          disabled={!data?.config}
          aria-label="Ajustar o horário de apagar"
        >
          <div className="sched-lbl">Apaga</div>
          <div className="sched-val">{data?.config?.light_off_time ?? "--:--"}</div>
        </button>
        <span className="sched-editar" aria-hidden>
          <IconPencil />
        </span>
      </div>
      <SettingsStatus feedback={meuAjuste} />

      <div className="mini-clock-row">
        <span className="mini-clock-lbl">Relógio do aquário</span>
        <span className="mini-clock-val">
          {s?.rtc.available ? (s.rtc.time ?? "--:--") : "--:--"}
        </span>
      </div>

      <button
        className="btn btn-light"
        disabled={!online || enviando}
        onClick={() => {
          const novo = !acesa;
          setOtimista(novo);
          onSend({ action: "light.set", on: novo });
        }}
      >
        <IconBulb />
        {acesa ? "Apagar luminária" : "Acender luminária"}
      </button>
      <div className="controle-ajuda" style={{ textAlign: "center" }}>
        Vale até a próxima virada do horário automático.
      </div>

      <CommandStatus feedback={meuFeedback} intervalMs={data?.config?.telemetry_interval_ms} />

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

      {editando && data?.config ? (
        <LightScheduleSheet
          onTime={data.config.light_on_time}
          offTime={data.config.light_off_time}
          saving={meuAjuste?.status === "saving"}
          onClose={fecharEditor}
          onSave={(on, off) => {
            void onSaveSettings({ light_on_time: on, light_off_time: off }, "light").then(
              (ok) => {
                if (ok) setEditando(false);
              },
            );
          }}
        />
      ) : null}
    </>
  );
}
