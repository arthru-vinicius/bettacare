"use client";

import { useState } from "react";

import { Sheet } from "@/components/Sheet";
import { formatDuration } from "@/lib/format";

/**
 * Editor do horário da luminária.
 *
 * Os campos são os seletores nativos de hora do sistema: no celular abrem a
 * roda de horas que a pessoa já conhece, e não há como digitar "25:70".
 */
export function LightScheduleSheet({
  onTime,
  offTime,
  saving,
  onSave,
  onClose,
}: {
  onTime: string;
  offTime: string;
  saving: boolean;
  onSave: (on: string, off: string) => void;
  onClose: () => void;
}) {
  const [liga, setLiga] = useState(onTime);
  const [apaga, setApaga] = useState(offTime);

  const valido = /^\d{2}:\d{2}$/.test(liga) && /^\d{2}:\d{2}$/.test(apaga);
  const iguais = valido && liga === apaga;
  const minutos = valido && !iguais ? duracao(liga, apaga) : null;
  const cruzaMeiaNoite = valido && apaga < liga;
  const mudou = liga !== onTime || apaga !== offTime;

  return (
    <Sheet
      title="Horário da luminária"
      subtitle="A automação acende e apaga sozinha nestes horários. Um toque no botão do app ou no físico vale até a próxima virada."
      onClose={onClose}
      footer={
        <>
          <button className="sheet-btn secundario" onClick={onClose} disabled={saving}>
            Cancelar
          </button>
          <button
            className="sheet-btn primario amber"
            disabled={!valido || iguais || !mudou || saving}
            onClick={() => onSave(liga, apaga)}
          >
            {saving ? "Salvando…" : "Salvar horário"}
          </button>
        </>
      }
    >
      <div className="campo-par">
        <label className="campo">
          <span className="campo-rotulo">Acende às</span>
          <input
            type="time"
            value={liga}
            onChange={(e) => setLiga(e.target.value)}
            required
          />
        </label>
        <label className="campo">
          <span className="campo-rotulo">Apaga às</span>
          <input
            type="time"
            value={apaga}
            onChange={(e) => setApaga(e.target.value)}
            required
          />
        </label>
      </div>

      {iguais ? (
        <div className="campo-erro">
          Os dois horários precisam ser diferentes — iguais, a luz ficaria acesa o dia
          inteiro.
        </div>
      ) : minutos !== null ? (
        <div className="campo-resumo">
          Acesa por <strong>{formatDuration(minutos)}</strong> por dia
          {cruzaMeiaNoite ? ", atravessando a meia-noite" : ""}.
        </div>
      ) : null}
    </Sheet>
  );
}

function duracao(liga: string, apaga: string): number {
  const min = (s: string) => Number(s.slice(0, 2)) * 60 + Number(s.slice(3, 5));
  const d = min(apaga) - min(liga);
  return d > 0 ? d : d + 24 * 60;
}
