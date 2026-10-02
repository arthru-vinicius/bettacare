"use client";

import type { SettingsFeedback } from "@/lib/useDevice";

/**
 * O desfecho de uma mudança de configuração — mesma linguagem do
 * `CommandStatus`, porque para quem usa é a mesma pergunta: "o aquário já
 * está fazendo o que eu pedi?".
 *
 * A diferença que importa mostrar é entre "salvo" e "aplicado": a
 * configuração vale assim que o servidor grava, mas o aquário só passa a
 * segui-la quando busca a versão nova.
 */
export function SettingsStatus({ feedback }: { feedback: SettingsFeedback | null }) {
  if (feedback === null) return null;

  switch (feedback.status) {
    case "saving":
      return (
        <div className="cmd-status pending" role="status" aria-live="polite">
          <span className="spinner-sm" />
          <span>Salvando…</span>
        </div>
      );
    case "applying":
      return (
        <div className="cmd-status pending" role="status" aria-live="polite">
          <span className="spinner-sm" />
          <span>
            Salvo. Enviando ao aquário…
            <span className="cmd-status-hint">Ele aplica na próxima conversa com o servidor.</span>
          </span>
        </div>
      );
    case "applied":
      return (
        <div className="cmd-status ok" role="status" aria-live="polite">
          <span aria-hidden>✓</span>
          <span>Aplicado no aquário</span>
        </div>
      );
    case "queued":
      return (
        <div className="cmd-status pending" role="status" aria-live="polite">
          <span aria-hidden>⏳</span>
          <span>
            Salvo no servidor
            <span className="cmd-status-hint">
              O aquário ainda não confirmou — aplica assim que voltar a se comunicar.
            </span>
          </span>
        </div>
      );
    case "fail":
      return (
        <div className="cmd-status fail" role="status" aria-live="polite">
          <span aria-hidden>✕</span>
          <span>
            Não foi possível salvar
            {feedback.reason ? (
              <span className="cmd-status-hint">{feedback.reason}</span>
            ) : null}
          </span>
        </div>
      );
  }
}
