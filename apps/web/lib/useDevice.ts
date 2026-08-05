"use client";

import {
  describeEventCode,
  targetOf,
  type CommandAction,
  type CommandItem,
  type CommandTarget,
  type OverviewResponse,
} from "@bettacare/contract";
import { useCallback, useEffect, useRef, useState } from "react";

import { ApiError, fetchCommands, fetchOverview, sendCommand } from "./api";

/** Intervalo de atualização quando a aba está visível. */
const POLL_MS = 4000;
/** Enquanto há comando em voo, acompanha de perto para o desfecho aparecer logo. */
const POLL_FAST_MS = 1500;

export interface CommandFeedback {
  id: number;
  /**
   * Qual tela emitiu o comando.
   *
   * Sem isto o aviso é global e vaza: uma falha da luminária apareceria também
   * na tela da ventoinha, onde não faz sentido nenhum.
   */
  target: CommandTarget;
  status: "pending" | "ok" | "fail";
  /** Motivo em português, quando falhou. */
  reason?: string;
  hint?: string;
}

/**
 * Estado do aquário, com polling.
 *
 * Polling e não WebSocket de propósito: são três usuários e acessos esparsos,
 * o dado só muda a cada 3 s de qualquer forma, e uma conexão persistente
 * custaria complexidade de reconexão para ganhar nada perceptível.
 *
 * **O polling para quando a aba está oculta.** Sem isso o app deixado aberto
 * no celular faria uma requisição a cada 4 s indefinidamente.
 */
export function useDevice() {
  const [data, setData] = useState<OverviewResponse | null>(null);
  const [commands, setCommands] = useState<CommandItem[]>([]);
  const [error, setError] = useState<string | null>(null);
  /**
   * O servidor respondeu, mas ainda não conhece nenhum aquário.
   *
   * É estado diferente de erro, e confundir os dois foi um bug real: enquanto o
   * ESP32 nunca tinha feito o primeiro POST, o `/api/overview` devolvia 404 e a
   * interface anunciava "sem contato com o servidor" — apontando o dedo para a
   * peça que estava funcionando.
   */
  const [deviceMissing, setDeviceMissing] = useState(false);
  const [loading, setLoading] = useState(true);
  const [refreshing, setRefreshing] = useState(false);
  const [feedback, setFeedback] = useState<CommandFeedback | null>(null);

  /** Ids que este cliente enviou e ainda aguardam desfecho. */
  const watching = useRef<Set<number>>(new Set());

  const load = useCallback(async (signal?: AbortSignal) => {
    try {
      const [overview, cmds] = await Promise.all([
        fetchOverview(signal),
        fetchCommands(20, signal),
      ]);
      setData(overview);
      setCommands(cmds);
      setError(null);
      setDeviceMissing(false);
      return { overview, cmds };
    } catch (e) {
      if ((e as Error).name === "AbortError") return null;

      // 404 é o servidor dizendo que respondeu e não conhece o aquário — o
      // oposto de estar fora do ar.
      if (e instanceof ApiError && e.status === 404) {
        setDeviceMissing(true);
        setError(null);
        return null;
      }

      setError(e instanceof Error ? e.message : "falha ao carregar");
      return null;
    } finally {
      setLoading(false);
    }
  }, []);

  // Resolve o feedback assim que o comando observado chega a um estado final.
  useEffect(() => {
    if (feedback === null || feedback.status !== "pending") return;

    const cmd = commands.find((c) => c.id === feedback.id);
    if (cmd === undefined) return;

    if (cmd.status === "acked") {
      watching.current.delete(cmd.id);
      setFeedback({ id: cmd.id, target: feedback.target, status: "ok" });
      return;
    }

    if (cmd.status === "rejected" || cmd.status === "expired") {
      watching.current.delete(cmd.id);
      setFeedback({
        id: cmd.id,
        target: feedback.target,
        status: "fail",
        ...describeFailure(cmd, data),
      });
    }
  }, [commands, feedback, data]);

  // Some com o aviso de sucesso; o de falha fica até o usuário agir de novo.
  useEffect(() => {
    if (feedback?.status !== "ok") return;
    const t = setTimeout(() => setFeedback(null), 3000);
    return () => clearTimeout(t);
  }, [feedback]);

  useEffect(() => {
    const controller = new AbortController();
    void load(controller.signal);

    let timer: ReturnType<typeof setInterval> | null = null;

    const start = () => {
      if (timer !== null) return;
      const ms = watching.current.size > 0 ? POLL_FAST_MS : POLL_MS;
      timer = setInterval(() => void load(), ms);
    };

    const stop = () => {
      if (timer === null) return;
      clearInterval(timer);
      timer = null;
    };

    const onVisibility = () => {
      if (document.visibilityState === "visible") {
        void load();
        start();
      } else {
        stop();
      }
    };

    if (document.visibilityState === "visible") start();
    document.addEventListener("visibilitychange", onVisibility);

    return () => {
      controller.abort();
      stop();
      document.removeEventListener("visibilitychange", onVisibility);
    };
    // `load` é estável (useCallback sem dependências).
  }, [load]);

  const refresh = useCallback(async () => {
    setRefreshing(true);
    await load();
    setRefreshing(false);
  }, [load]);

  const send = useCallback(
    async (action: CommandAction) => {
      setFeedback(null);
      const target = targetOf(action);
      try {
        const id = await sendCommand(action);
        watching.current.add(id);
        setFeedback({ id, target, status: "pending" });
        await load();
      } catch (e) {
        setFeedback({
          id: -1,
          target,
          status: "fail",
          reason: "Não foi possível falar com o servidor",
          hint: e instanceof Error ? e.message : undefined,
        });
      }
    },
    [load],
  );

  return {
    data,
    commands,
    error,
    deviceMissing,
    loading,
    refreshing,
    feedback,
    refresh,
    send,
    dismissFeedback: () => setFeedback(null),
  };
}

/**
 * Converte o desfecho de um comando em uma frase que explica o que houve.
 *
 * Cada frase vem de um estado distinto, não de heurística: "expirou" com o
 * dispositivo offline é uma causa diferente de "expirou" com ele online, e o
 * usuário precisa distinguir para saber se o problema é a rede ou o aquário.
 */
function describeFailure(
  cmd: CommandItem,
  data: OverviewResponse | null,
): { reason: string; hint?: string } {
  if (cmd.status === "rejected") {
    // O catálogo de códigos existe justamente para esta tradução: mostrar
    // `light.gpio_fault` cru ao usuário não diz nada sobre o que fazer.
    const info = cmd.error_code ? describeEventCode(cmd.error_code) : undefined;
    return {
      reason: info?.label ?? "O dispositivo recusou o comando",
      hint: info?.hint ?? cmd.error_code ?? undefined,
    };
  }

  if (data !== null && !data.device.online) {
    const desde = data.device.last_seen_at
      ? new Date(data.device.last_seen_at).toLocaleTimeString("pt-BR", {
          hour: "2-digit",
          minute: "2-digit",
        })
      : null;
    return {
      reason: desde
        ? `Dispositivo sem contato desde ${desde}`
        : "Dispositivo offline",
      hint: "O comando será entregue assim que ele voltar a falar com o servidor.",
    };
  }

  return {
    reason: "O comando expirou sem confirmação",
    hint: "O dispositivo recebeu o comando mas nunca confirmou. Pode ter reiniciado no meio.",
  };
}
