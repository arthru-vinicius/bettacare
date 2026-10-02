"use client";

import { useEffect, useEffectEvent, useId, useRef } from "react";
import { createPortal } from "react-dom";

/**
 * Folha que sobe do rodapé — o editor dos ajustes (horário da luz, limites da
 * ventoinha).
 *
 * Folha e não tela nova: o ajuste é uma decisão curta tomada olhando o estado
 * atual, e manter a tela de origem visível por trás preserva o contexto. O
 * botão de confirmar fica embaixo, no alcance do polegar.
 *
 * Fecha com o fundo, com Esc e com o botão "Cancelar"; nunca sozinha, para
 * não jogar fora o que a pessoa estava editando.
 */
export function Sheet({
  title,
  subtitle,
  onClose,
  children,
  footer,
}: {
  title: string;
  subtitle?: string;
  onClose: () => void;
  children: React.ReactNode;
  footer: React.ReactNode;
}) {
  const id = useId();
  const painel = useRef<HTMLDivElement>(null);
  // Evento de efeito, não dependência: a tela de origem re-renderiza a cada
  // consulta (2 s) com um `onClose` novo, e um efeito que dependesse dele
  // devolveria o foco ao painel no meio da digitação. O `useEffectEvent`
  // sempre chama o `onClose` mais recente sem reexecutar o efeito.
  const fechar = useEffectEvent(onClose);

  useEffect(() => {
    const anterior = document.activeElement as HTMLElement | null;
    painel.current?.focus();
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") fechar();
    };
    document.addEventListener("keydown", onKey);
    return () => {
      document.removeEventListener("keydown", onKey);
      anterior?.focus?.();
    };
  }, []);

  /*
   * Portal para o `body`, não renderizada no lugar: as telas animam com
   * `transform`, e um ancestral com `transform` vira o referencial do
   * `position: fixed` e prende o `z-index` — a folha ficava confinada à tela,
   * com os botões de salvar escondidos atrás da barra de navegação.
   */
  return createPortal(
    <div className="sheet-fundo" role="presentation" onClick={onClose}>
      <div
        ref={painel}
        className="sheet"
        role="dialog"
        aria-modal="true"
        aria-labelledby={`${id}-titulo`}
        tabIndex={-1}
        onClick={(e) => e.stopPropagation()}
      >
        <div className="sheet-alca" aria-hidden />
        <h2 className="sheet-titulo" id={`${id}-titulo`}>
          {title}
        </h2>
        {subtitle ? <p className="sheet-sub">{subtitle}</p> : null}
        <div className="sheet-corpo">{children}</div>
        <div className="sheet-rodape">{footer}</div>
      </div>
    </div>,
    document.body,
  );
}
