"use client";

import type { CommandFeedback } from "@/lib/useDevice";

/**
 * O desfecho de um comando, em português direto.
 *
 * É a resposta à pergunta que motivou toda a Fase 2: *por que a luminária não
 * acendeu quando eu apertei o botão?* O botão não volta ao normal sozinho —
 * ele passa por enviando → confirmado, ou enviando → falhou com a razão.
 */
export function CommandStatus({ feedback }: { feedback: CommandFeedback | null }) {
  if (feedback === null) return null;

  if (feedback.status === "pending") {
    return (
      <div className="cmd-status pending">
        <span className="spinner-sm" />
        <span>
          Enviando ao dispositivo…
          <span className="cmd-status-hint">
            Ele busca a fila a cada 3 segundos.
          </span>
        </span>
      </div>
    );
  }

  if (feedback.status === "ok") {
    return (
      <div className="cmd-status ok">
        <span aria-hidden>✓</span>
        <span>Confirmado pelo dispositivo</span>
      </div>
    );
  }

  return (
    <div className="cmd-status fail">
      <span aria-hidden>✕</span>
      <span>
        {feedback.reason}
        {feedback.hint ? (
          <span className="cmd-status-hint">{feedback.hint}</span>
        ) : null}
      </span>
    </div>
  );
}
