"use client";

import type { CommandAction, OverviewResponse } from "@bettacare/contract";
import { useEffect, useState } from "react";

import { CommandStatus } from "@/components/CommandStatus";
import { IconFeeder } from "@/components/icons";
import { secondsAgo } from "@/lib/format";
import type { CommandFeedback } from "@/lib/useDevice";

const HOUR_MIN = 0;
const HOUR_MAX = 23;
const GRAINS_MIN = 1;
const GRAINS_MAX = 20;

interface FormState {
  hour1: number;
  hour2: number;
  grains: number;
  auto: boolean;
}

const FACTORY_DEFAULT: FormState = { hour1: 5, hour2: 17, grains: 5, auto: true };

/**
 * Tela do alimentador de precisão — módulo opcional, ligado ao ESP32 por um
 * ESP32-C3 próprio (ver `docs/pinagem-alimentador-modulo.md`). Ele não fica
 * ligado o tempo todo por desenho, então "não conectado" é o estado comum
 * desta tela, não um erro — é por isso que ela nunca usa `.pill.danger` nem
 * um tom de alarme para isso.
 */
export function Feeder({
  data,
  feedback,
  onSend,
}: {
  data: OverviewResponse | null;
  feedback: CommandFeedback | null;
  onSend: (a: CommandAction) => void;
}) {
  const feeder = data?.state?.feeder ?? null;
  const online = data?.device.online ?? false;
  const conectado = online && (feeder?.connected ?? false);
  const meuFeedback = feedback?.target === "feeder" ? feedback : null;
  const enviando = meuFeedback?.status === "pending";

  // Nulo é "nunca ouvi falar desse módulo" — diferente de `connected: false`,
  // que é "conheço a agenda dele, só não está respondendo agora". A primeira
  // tela nem tenta mostrar controles; a segunda mostra a última agenda
  // conhecida, desabilitada.
  const nuncaDetectado = feeder === null;

  // Mesmo padrão do slider da ventoinha (`arrastando`): enquanto há edição
  // local não confirmada, o polling não pode puxar o formulário de volta.
  const [local, setLocal] = useState(false);
  const [form, setForm] = useState<FormState>(FACTORY_DEFAULT);

  useEffect(() => {
    if (local) return;
    setForm({
      hour1: feeder?.hour1 ?? FACTORY_DEFAULT.hour1,
      hour2: feeder?.hour2 ?? FACTORY_DEFAULT.hour2,
      grains: feeder?.grains_per_feeding ?? FACTORY_DEFAULT.grains,
      auto: feeder?.auto_enabled ?? FACTORY_DEFAULT.auto,
    });
  }, [feeder, local]);

  // Some com o formulário "sujo" assim que o servidor confirma a mudança.
  useEffect(() => {
    if (meuFeedback?.status === "ok") setLocal(false);
  }, [meuFeedback]);

  if (nuncaDetectado) {
    return (
      <>
        <div className="screen-title">Alimentador</div>
        <div className="feeder-empty">
          <div className="hero-icon">
            <IconFeeder />
          </div>
          <div className="feeder-empty-title">Não conectado</div>
          <div className="feeder-empty-sub">
            Nenhum módulo de alimentação de precisão foi detectado ainda. É um
            acessório opcional — o resto do aquário funciona normalmente sem
            ele.
          </div>
        </div>
      </>
    );
  }

  const step = (field: "hour1" | "hour2", delta: number) => {
    setLocal(true);
    setForm((f) => ({
      ...f,
      [field]: wrap(f[field] + delta, HOUR_MIN, HOUR_MAX),
    }));
  };

  const stepGrains = (delta: number) => {
    setLocal(true);
    setForm((f) => ({
      ...f,
      grains: clamp(f.grains + delta, GRAINS_MIN, GRAINS_MAX),
    }));
  };

  const salvar = () => {
    onSend({
      action: "feeder.set_config",
      hour1: form.hour1,
      hour2: form.hour2,
      grains_per_feeding: form.grains,
      auto_enabled: form.auto,
    });
  };

  return (
    <>
      <div className="screen-title">Alimentador</div>

      <div className={`state-hero ${conectado ? "feeder-on" : ""}`}>
        <div className="hero-icon">
          <IconFeeder />
        </div>
        <div className="hero-state">
          {conectado ? "CONECTADO" : "NÃO CONECTADO"}
        </div>
        <div className="hero-sub">
          {conectado
            ? "Ele não fica ligado o tempo todo por desenho — isto é normal"
            : "Mostrando a última agenda conhecida"}
        </div>
      </div>

      <div className="card" style={{ marginBottom: 12 }}>
        <div className="c-label">Última alimentação</div>
        {feeder.last_feed_age_s == null ? (
          <div className="c-val">—</div>
        ) : (
          <>
            <div className="c-val">
              {feeder.last_feed_confirmed ?? 0}/{feeder.last_feed_requested ?? 0}{" "}
              grãos
            </div>
            <div className="c-sub">
              {secondsAgo(feeder.last_feed_age_s)}
              {feeder.last_feed_ok === false ? " · incompleta" : ""}
            </div>
          </>
        )}
      </div>

      {feeder.last_feed_ok === false ? (
        <div className="health-hint">
          O sensor confirmou menos grãos do que foi pedido na última
          alimentação — pode ter ficado um grão preso no tubo. Vale checar o
          reservatório.
        </div>
      ) : null}

      <div className="sched-row">
        <div className="sched-col">
          <div className="sched-lbl">1ª refeição</div>
          <NumField
            value={form.hour1}
            suffix="h"
            disabled={!conectado || enviando}
            onDec={() => step("hour1", -1)}
            onInc={() => step("hour1", 1)}
          />
        </div>
        <div className="sched-sep" />
        <div className="sched-col">
          <div className="sched-lbl">2ª refeição</div>
          <NumField
            value={form.hour2}
            suffix="h"
            disabled={!conectado || enviando}
            onDec={() => step("hour2", -1)}
            onInc={() => step("hour2", 1)}
          />
        </div>
      </div>

      <div className="card" style={{ marginBottom: 12 }}>
        <div className="row-between">
          <span className="c-label" style={{ marginBottom: 0 }}>
            Grãos por refeição
          </span>
          <NumField
            value={form.grains}
            disabled={!conectado || enviando}
            onDec={() => stepGrains(-1)}
            onInc={() => stepGrains(1)}
          />
        </div>
      </div>

      <div className="card" style={{ marginBottom: 12 }}>
        <div className="row-between">
          <span className="c-label" style={{ marginBottom: 0 }}>
            Alimentação automática
          </span>
          <button
            className={`toggle ${form.auto ? "on" : ""}`}
            disabled={!conectado || enviando}
            aria-pressed={form.auto}
            aria-label="Alimentação automática"
            onClick={() => {
              setLocal(true);
              setForm((f) => ({ ...f, auto: !f.auto }));
            }}
          >
            <span className="toggle-thumb" />
          </button>
        </div>
      </div>

      {local ? (
        <button
          className="btn btn-feeder"
          disabled={!conectado || enviando}
          onClick={salvar}
        >
          Salvar agenda
        </button>
      ) : null}

      <button
        className={`btn btn-feeder ${local ? "secondary" : ""}`}
        disabled={!conectado || enviando}
        onClick={() => onSend({ action: "feeder.feed_now" })}
      >
        <IconFeeder />
        Alimentar agora
      </button>

      <CommandStatus feedback={meuFeedback} />

      {!conectado && meuFeedback === null ? (
        <div className="cmd-status fail">
          <span aria-hidden>✕</span>
          <span>
            Módulo não conectado
            <span className="cmd-status-hint">
              Os controles voltam a funcionar assim que ele reconectar.
            </span>
          </span>
        </div>
      ) : null}
    </>
  );
}

function NumField({
  value,
  suffix,
  disabled,
  onDec,
  onInc,
}: {
  value: number;
  suffix?: string;
  disabled: boolean;
  onDec: () => void;
  onInc: () => void;
}) {
  return (
    <div className="num-field">
      <button disabled={disabled} aria-label="Diminuir" onClick={onDec}>
        −
      </button>
      <span className="num-field-val">
        {String(value).padStart(2, "0")}
        {suffix ?? ""}
      </span>
      <button disabled={disabled} aria-label="Aumentar" onClick={onInc}>
        +
      </button>
    </div>
  );
}

function wrap(v: number, min: number, max: number): number {
  const range = max - min + 1;
  return min + (((v - min) % range) + range) % range;
}

function clamp(v: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, v));
}
