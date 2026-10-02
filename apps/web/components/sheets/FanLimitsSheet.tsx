"use client";

import { useState } from "react";

import { Sheet } from "@/components/Sheet";
import { formatTemp } from "@/lib/format";

/** Faixa aceita pelo contrato (`deviceConfigSchema`). */
const MIN_C = 15;
const MAX_C = 40;
const PASSO = 0.5;
/**
 * Folga mínima entre ligar e desligar. O contrato só exige "menor"; meio grau
 * de histerese é o mínimo que evita a ventoinha liga-desliga com o ruído do
 * último dígito do sensor.
 */
const FOLGA = 0.5;
/** Fixo no firmware (`FAN_COOLDOWN_MIN`), não vem da configuração. */
const COOLDOWN_MIN = 30;

/**
 * Editor dos limites da ventoinha no modo automático.
 *
 * O texto explica o comportamento inteiro, não só os dois números: "desliga
 * em 27,5 °C" sozinho faz esperar que ela pare na hora, e ela ainda gira meia
 * hora em velocidade baixa depois disso.
 */
export function FanLimitsSheet({
  triggerC,
  offC,
  currentC,
  saving,
  onSave,
  onClose,
}: {
  triggerC: number;
  offC: number;
  currentC: number | null;
  saving: boolean;
  onSave: (trigger: number, off: number) => void;
  onClose: () => void;
}) {
  const [liga, setLiga] = useState(triggerC);
  const [desliga, setDesliga] = useState(offC);

  const passo = (v: number, delta: number) =>
    Math.min(MAX_C, Math.max(MIN_C, Math.round((v + delta) / PASSO) * PASSO));

  const valido = desliga <= liga - FOLGA;
  const mudou = liga !== triggerC || desliga !== offC;

  return (
    <Sheet
      title="Limites da ventoinha"
      subtitle="Valem no modo automático. Ligar ou desligar pelo app passa para o modo manual até você voltar ao automático."
      onClose={onClose}
      footer={
        <>
          <button className="sheet-btn secundario" onClick={onClose} disabled={saving}>
            Cancelar
          </button>
          <button
            className="sheet-btn primario"
            disabled={!valido || !mudou || saving}
            onClick={() => onSave(liga, desliga)}
          >
            {saving ? "Salvando…" : "Salvar limites"}
          </button>
        </>
      }
    >
      {currentC !== null ? (
        <div className="campo-resumo" style={{ marginTop: 0 }}>
          Água agora: <strong>{formatTemp(currentC)} °C</strong>
        </div>
      ) : null}

      <div className="limite">
        <div>
          <div className="campo-rotulo">Liga acima de</div>
          <div className="limite-ajuda">A ventoinha arranca quando a água passa disto.</div>
        </div>
        <Passo
          valor={liga}
          onMenos={() => setLiga((v) => passo(v, -PASSO))}
          onMais={() => setLiga((v) => passo(v, PASSO))}
          rotulo="acionamento"
        />
      </div>

      <div className="limite">
        <div>
          <div className="campo-rotulo">Desliga abaixo de</div>
          <div className="limite-ajuda">
            Depois de esfriar até aqui, ainda gira {COOLDOWN_MIN} min devagar e então para.
          </div>
        </div>
        <Passo
          valor={desliga}
          onMenos={() => setDesliga((v) => passo(v, -PASSO))}
          onMais={() => setDesliga((v) => passo(v, PASSO))}
          rotulo="desligamento"
        />
      </div>

      {!valido ? (
        <div className="campo-erro">
          O desligamento precisa ficar pelo menos {formatTemp(FOLGA)} °C abaixo do
          acionamento — sem essa folga ela liga e desliga sem parar.
        </div>
      ) : null}
    </Sheet>
  );
}

function Passo({
  valor,
  onMenos,
  onMais,
  rotulo,
}: {
  valor: number;
  onMenos: () => void;
  onMais: () => void;
  rotulo: string;
}) {
  return (
    <div className="num-field">
      <button aria-label={`Diminuir ${rotulo}`} onClick={onMenos} disabled={valor <= MIN_C}>
        −
      </button>
      <span className="num-field-val limite-val">{formatTemp(valor)}°</span>
      <button aria-label={`Aumentar ${rotulo}`} onClick={onMais} disabled={valor >= MAX_C}>
        +
      </button>
    </div>
  );
}
