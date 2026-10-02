"use client";

import type { CommandAction, DeviceConfig, OverviewResponse } from "@bettacare/contract";
import { useCallback, useEffect, useRef, useState } from "react";

import { CommandStatus } from "@/components/CommandStatus";
import { IconAuto, IconFan, IconPencil, IconPower } from "@/components/icons";
import { SettingsStatus } from "@/components/SettingsStatus";
import { FanLimitsSheet } from "@/components/sheets/FanLimitsSheet";
import { TempHistory } from "@/components/TempHistory";
import { formatInt, formatTemp } from "@/lib/format";
import type { CommandFeedback, SettingsArea, SettingsFeedback } from "@/lib/useDevice";

const MODO: Record<string, string> = {
  auto: "Automático pela temperatura",
  manual: "Manual",
  manual_off: "Desligada manualmente",
  failsafe: "Modo de segurança — sem leitura de temperatura",
};

/** Teclas que mudam o valor de um `<input type="range">`. */
const TECLAS_DO_SLIDER = new Set([
  "ArrowLeft",
  "ArrowRight",
  "ArrowUp",
  "ArrowDown",
  "Home",
  "End",
  "PageUp",
  "PageDown",
]);

/**
 * O que a tela mostra enquanto o comando não volta confirmado. Só os campos
 * que o comando define: "ligar" não sabe a velocidade (o aquário volta à
 * última velocidade manual), "automático" não sabe se ela vai girar.
 */
interface Otimista {
  ligada?: boolean;
  modo?: string;
  pct?: number;
  desde: number;
}

/** Sem confirmação nesse tempo, volta a mostrar só o que o aquário reporta. */
const OTIMISTA_MAX_MS = 15_000;

export function Fan({
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
  // Só os avisos desta tela: um erro da luminária não é assunto aqui.
  const meuFeedback = feedback?.target === "fan" ? feedback : null;
  const meuAjuste = settingsFeedback?.area === "fan" ? settingsFeedback : null;
  const enviando = meuFeedback?.status === "pending";
  const travada = data?.components.find((c) => c.comp === "fan")?.status === "fault";

  const [otimista, setOtimista] = useState<Otimista | null>(null);
  const [editando, setEditando] = useState(false);

  useEffect(() => {
    if (otimista === null) return;
    if (meuFeedback?.status === "fail" || Date.now() - otimista.desde > OTIMISTA_MAX_MS) {
      setOtimista(null);
      return;
    }
    if (s === null) return;
    const modoAlcancado =
      otimista.modo === undefined ||
      s.fan.mode === otimista.modo ||
      // "Automático" sem leitura de temperatura vira modo de segurança — é o
      // mesmo pedido atendido.
      (otimista.modo === "auto" && s.fan.mode === "failsafe");
    const alcancou =
      modoAlcancado &&
      (otimista.pct === undefined || s.fan.speed_percent === otimista.pct) &&
      (otimista.ligada === undefined || s.fan.on === otimista.ligada);
    if (alcancou) setOtimista(null);
  }, [s, meuFeedback, otimista]);

  const ligada =
    otimista?.ligada ?? (otimista?.pct !== undefined ? otimista.pct > 0 : (s?.fan.on ?? false));
  const modo = otimista?.modo ?? s?.fan.mode ?? "auto";
  const manual = modo === "manual" || modo === "manual_off";

  /*
   * O slider segue o servidor, exceto enquanto o dedo está nele — sem isso, o
   * polling puxaria o controle de volta no meio do arrasto. E só manda
   * comando se o valor mudou: um toque parado no controle não deve tirar a
   * ventoinha do automático.
   */
  const [arrastando, setArrastando] = useState(false);
  const [valor, setValor] = useState(0);
  const valorInicial = useRef<number | null>(null);

  useEffect(() => {
    if (arrastando) return;
    setValor(otimista?.pct ?? s?.fan.speed_percent ?? 0);
  }, [s, arrastando, otimista]);

  const comandar = (acao: CommandAction, previsto: Omit<Otimista, "desde">) => {
    setOtimista({ ...previsto, desde: Date.now() });
    onSend(acao);
  };

  const soltarSlider = () => {
    const inicial = valorInicial.current;
    valorInicial.current = null;
    setArrastando(false);
    if (inicial === null || inicial === valor) return;
    comandar(
      { action: "fan.set_speed", percent: valor },
      { pct: valor, ligada: valor > 0, modo: valor > 0 ? "manual" : "manual_off" },
    );
  };

  const fecharEditor = useCallback(() => setEditando(false), []);

  return (
    <>
      <div className="screen-title">Ventoinha e temperatura</div>

      <div className={`state-hero ${ligada ? "fan-on" : ""}`}>
        <div className="hero-icon">
          <IconFan />
        </div>
        <div className="hero-state">{ligada ? "LIGADA" : "DESLIGADA"}</div>
        <div className="hero-sub">
          {s === null
            ? "Aguardando dados"
            : `${MODO[modo] ?? modo}${ligada && s.fan.rpm > 0 ? ` · ${formatInt(s.fan.rpm)} rpm` : ""}`}
        </div>
      </div>

      {/*
        O tacômetro é a única realimentação real do sistema inteiro. PWM acima
        de zero com rotação zerada significa fisicamente uma coisa só.
      */}
      {travada ? (
        <div className="health-hint">
          Há comando de girar mas o tacômetro lê zero — a ventoinha não está
          girando. Cabo solto, rolamento travado ou fonte caída.
        </div>
      ) : null}

      <button
        className="card card-toque"
        onClick={() => setEditando(true)}
        disabled={!data?.config}
        aria-label="Ajustar os limites de temperatura da ventoinha"
      >
        <div className="row-between">
          <div>
            <div className="c-label">Temperatura da água</div>
            <div className="c-val">
              {formatTemp(s?.temperature.celsius)}
              {s?.temperature.celsius != null ? <span className="c-unidade"> °C</span> : null}
            </div>
            {data?.config ? (
              <div className="c-sub">
                Liga acima de {formatTemp(data.config.fan_trigger_c)} °C · desliga abaixo de{" "}
                {formatTemp(data.config.fan_off_c)} °C
              </div>
            ) : null}
          </div>
          <span className="toque-dica">
            <IconPencil />
            Ajustar
          </span>
        </div>
      </button>
      <SettingsStatus feedback={meuAjuste} />

      <div className="slider-card controle">
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
          aria-label="Potência da ventoinha"
          disabled={!online || enviando}
          onPointerDown={() => {
            valorInicial.current = valor;
          }}
          onKeyDown={(e) => {
            if (TECLAS_DO_SLIDER.has(e.key) && valorInicial.current === null) {
              valorInicial.current = valor;
            }
          }}
          onChange={(e) => {
            setArrastando(true);
            setValor(Number(e.target.value));
          }}
          onPointerUp={soltarSlider}
          onKeyUp={(e) => {
            if (TECLAS_DO_SLIDER.has(e.key)) soltarSlider();
          }}
        />

        <div className="botoes-coluna">
          <button
            className={`btn ${ligada ? "btn-desligar" : "btn-fan"}`}
            disabled={!online || enviando}
            onClick={() =>
              ligada
                ? comandar(
                    { action: "fan.set_speed", percent: 0 },
                    { pct: 0, ligada: false, modo: "manual_off" },
                  )
                : comandar({ action: "fan.set_mode", mode: "manual" }, { ligada: true, modo: "manual" })
            }
          >
            <IconPower />
            {ligada ? "Desligar ventoinha" : "Ligar ventoinha"}
          </button>

          <button
            className="btn btn-secundario"
            disabled={!manual || !online || enviando}
            onClick={() => comandar({ action: "fan.set_mode", mode: "auto" }, { modo: "auto" })}
          >
            <IconAuto />
            Voltar ao automático
          </button>
        </div>

        <div className="controle-ajuda">
          {manual
            ? "No manual, ela fica como você deixou. “Voltar ao automático” devolve o controle à temperatura."
            : "No automático, ela segue a temperatura. Ligar, desligar ou mexer na potência passa para o manual."}
        </div>
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

      <TempHistory />

      {editando && data?.config ? (
        <FanLimitsSheet
          triggerC={data.config.fan_trigger_c}
          offC={data.config.fan_off_c}
          currentC={s?.temperature.celsius ?? null}
          saving={meuAjuste?.status === "saving"}
          onClose={fecharEditor}
          onSave={(trigger, off) => {
            void onSaveSettings({ fan_trigger_c: trigger, fan_off_c: off }, "fan").then((ok) => {
              if (ok) setEditando(false);
            });
          }}
        />
      ) : null}
    </>
  );
}
