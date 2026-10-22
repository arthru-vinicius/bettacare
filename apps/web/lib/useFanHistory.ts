"use client";

import type { FanHistoryResponse, HistoryRange } from "@bettacare/contract";
import { useCallback, useEffect, useState } from "react";

import { fetchFanHistory } from "./api";

/** De quanto em quanto tempo o histórico se atualiza enquanto a aba está à vista. */
const REFRESH_MS = 60_000;

export interface FanHistoryState {
  /**
   * A última resposta boa. **Fica na tela enquanto a próxima carrega** — trocar
   * de período não pisca nem pula o layout: o gráfico antigo esmaece até o novo
   * chegar.
   */
  dados: FanHistoryResponse | null;
  carregando: boolean;
  erro: boolean;
  recarregar: () => void;
}

/**
 * Histórico da ventoinha e da temperatura de um período.
 *
 * Só busca com a aba à vista (`ativo`) e a página visível: as telas ficam todas
 * montadas, e sem isso o app aberto em segundo plano pediria 30 dias de dados a
 * cada minuto para ninguém.
 */
export function useFanHistory(range: HistoryRange, ativo: boolean): FanHistoryState {
  const [dados, setDados] = useState<FanHistoryResponse | null>(null);
  const [carregando, setCarregando] = useState(false);
  const [erro, setErro] = useState(false);
  const [rodada, setRodada] = useState(0);

  const recarregar = useCallback(() => setRodada((n) => n + 1), []);

  useEffect(() => {
    if (!ativo) return;
    let encerrado = false;
    let ac: AbortController | null = null;

    const carregar = () => {
      ac?.abort();
      ac = new AbortController();
      setCarregando(true);
      fetchFanHistory(range, ac.signal)
        .then((r) => {
          if (encerrado) return;
          setDados(r);
          setErro(false);
        })
        .catch((e: unknown) => {
          if (encerrado || (e as Error).name === "AbortError") return;
          setErro(true);
        })
        .finally(() => {
          if (!encerrado) setCarregando(false);
        });
    };

    const seVisivel = () => {
      if (document.visibilityState === "visible") carregar();
    };

    carregar();
    const timer = setInterval(seVisivel, REFRESH_MS);
    document.addEventListener("visibilitychange", seVisivel);
    return () => {
      encerrado = true;
      ac?.abort();
      clearInterval(timer);
      document.removeEventListener("visibilitychange", seVisivel);
    };
  }, [range, ativo, rodada]);

  return { dados, carregando, erro, recarregar };
}
