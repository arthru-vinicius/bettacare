"use client";

import { formatInterval, formatTime } from "@/lib/format";
import type { CommandFeedback } from "@/lib/useDevice";

/**
 * O desfecho de um comando, em português direto.
 *
 * É a resposta à pergunta que motivou toda a Fase 2: *por que a luminária não
 * acendeu quando eu apertei o botão?* O botão não volta ao normal sozinho —
 * ele passa por enviando → confirmado, ou enviando → falhou com a razão.
 */
export function CommandStatus({
  feedback,
  intervalMs,
}: {
  feedback: CommandFeedback | null;
  /** Intervalo de telemetria configurado — é o tempo máximo até o aquário buscar o comando. */
  intervalMs?: number | undefined;
}) {
  if (feedback === null) return null;

  if (feedback.status === "pending") {
    return (
      <div className="cmd-status pending" role="status" aria-live="polite">
        <span className="spinner-sm" />
        <span>
          Enviando ao aquário…
          <span className="cmd-status-hint">
            Ele busca os comandos a cada {formatInterval(intervalMs)}.
          </span>
        </span>
      </div>
    );
  }

  if (feedback.status === "queued") {
    // Espera, não falha: com o Wi-Fi do aquário caindo por alguns minutos
    // várias vezes ao dia, o comando fica no servidor e sai na volta.
    const desde = feedback.offlineSince ? formatTime(feedback.offlineSince) : null;
    const ate = feedback.expiresAt ? formatTime(feedback.expiresAt) : null;
    return (
      <div className="cmd-status queued" role="status" aria-live="polite">
        <span aria-hidden>⏳</span>
        <span>
          Na fila — o aquário está sem contato{desde ? ` desde ${desde}` : ""}
          <span className="cmd-status-hint">
            {ate
              ? `O comando fica guardado até ${ate} e é executado assim que ele voltar.`
              : "O comando é executado assim que ele voltar."}
          </span>
        </span>
      </div>
    );
  }

  if (feedback.status === "ok") {
    return (
      <div className="cmd-status ok" role="status" aria-live="polite">
        <span aria-hidden>✓</span>
        <span>
          {feedback.wasQueued ? "Executado pelo aquário quando ele voltou" : "Confirmado pelo aquário"}
        </span>
      </div>
    );
  }

  return (
    <div className="cmd-status fail" role="status" aria-live="polite">
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
