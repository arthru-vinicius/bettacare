"use client";

import {
  describeEventCode,
  targetOf,
  type CommandAction,
  type CommandItem,
  type CommandTarget,
  type DeviceConfig,
  type OverviewResponse,
} from "@bettacare/contract";
import { useCallback, useEffect, useRef, useState } from "react";

import { ApiError, fetchCommands, fetchOverview, saveConfig, sendCommand } from "./api";

/**
 * Intervalo de atualização quando a aba está visível.
 *
 * 2 s (eram 4): com o aquário falando a cada 1 s, esperar 4 s por uma
 * leitura nova fazia o app parecer mais lento do que o sistema é. Continua
 * parando com a aba oculta, então o custo só existe com alguém olhando.
 */
const POLL_MS = 2000;
/** Enquanto há comando ou configuração em voo, acompanha de perto. */
const POLL_FAST_MS = 600;
/** Teto do recuo progressivo quando o servidor não responde (UPGRADE/06, P6). */
const POLL_BACKOFF_MAX_MS = 60_000;
/** Quanto esperar o aquário confirmar uma configuração nova antes de desistir de acompanhar. */
const SETTINGS_APPLY_TIMEOUT_MS = 30_000;

/** Em que parte da interface a configuração foi mudada — o aviso aparece só lá. */
export type SettingsArea = "light" | "fan" | "system";

export interface SettingsFeedback {
  area: SettingsArea;
  /**
   * `saving`: indo para o servidor. `applying`: salva, esperando o aquário
   * buscar. `applied`: o aquário confirmou que usa a versão nova. `queued`:
   * salva, mas o aquário está sem contato — aplica quando voltar. `fail`: o
   * servidor recusou ou não respondeu.
   */
  status: "saving" | "applying" | "applied" | "queued" | "fail";
  reason?: string;
}

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
 * o dado só muda no ritmo da telemetria de qualquer forma, e uma conexão
 * persistente custaria complexidade de reconexão para ganhar pouco.
 *
 * **O polling para quando a aba está oculta.** Sem isso o app deixado aberto
 * no celular faria requisições indefinidamente.
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
  /** Falhas seguidas do polling — alimenta o recuo progressivo (UPGRADE/06, P6). */
  const pollFailures = useRef(0);
  const [diagnosticRunning, setDiagnosticRunning] = useState(false);
  /** Enquanto esperamos um relatório novo: quando pedimos, e qual era o anterior. */
  const diagnosticWaitingSince = useRef<{ since: number; previousRanAt: string | null } | null>(
    null,
  );
  /** Preenchida pelo efeito de polling; `send()` a usa para reagir na hora. */
  const restartPolling = useRef<() => void>(() => {});

  const [settingsFeedback, setSettingsFeedback] = useState<SettingsFeedback | null>(null);
  /** Versão que acabamos de gravar e o aquário ainda não confirmou. */
  const settingsWaiting = useRef<{ version: number; since: number; area: SettingsArea } | null>(
    null,
  );

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
      // Se vínhamos de um recuo, volta ao ritmo normal imediatamente — não
      // espera a próxima troca de aba para perceber que o servidor voltou.
      if (pollFailures.current > 0) {
        pollFailures.current = 0;
        restartPolling.current();
      }
      return { overview, cmds };
    } catch (e) {
      if ((e as Error).name === "AbortError") return null;

      // 404 é o servidor dizendo que respondeu e não conhece o aquário — o
      // oposto de estar fora do ar.
      if (e instanceof ApiError && e.status === 404) {
        setDeviceMissing(true);
        setError(null);
        pollFailures.current = 0;
        return null;
      }

      pollFailures.current += 1;
      setError(describeLoadError(e));
      restartPolling.current();
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
      restartPolling.current(); // volta ao ritmo normal se não há mais nada em voo
      setFeedback({ id: cmd.id, target: feedback.target, status: "ok" });
      return;
    }

    if (cmd.status === "rejected" || cmd.status === "expired") {
      watching.current.delete(cmd.id);
      restartPolling.current();
      setFeedback({
        id: cmd.id,
        target: feedback.target,
        status: "fail",
        ...describeFailure(cmd, data),
      });
      return;
    }

    /**
     * Ainda `queued` com o dispositivo offline (UPGRADE/06, P4; UPGRADE/04,
     * S4). Um comando nesse estado nunca vira "expired" sozinho — ele está
     * legitimamente esperando o dispositivo voltar. Sem isto, o spinner
     * "Enviando ao dispositivo... Ele busca a fila a cada 3 segundos" girava
     * para sempre, prometendo uma entrega que não ia acontecer.
     */
    if (cmd.status === "queued" && data !== null && !data.device.online) {
      watching.current.delete(cmd.id);
      restartPolling.current();
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

    const stop = () => {
      if (timer === null) return;
      clearInterval(timer);
      timer = null;
    };

    /**
     * (Re)inicia o intervalo com o ritmo certo para agora.
     *
     * Antes só era chamada na montagem e ao trocar de aba — então
     * `POLL_FAST_MS` nunca era alcançado no fluxo normal de enviar um
     * comando, porque alterar `watching.current` (um `useRef`) não dispara
     * o efeito de novo (UPGRADE/06, P1). Expor esta função por `ref` deixa
     * `send()` chamá-la explicitamente assim que enfileira algo.
     */
    const start = () => {
      stop();
      if (document.visibilityState !== "visible") return;

      const emVoo = watching.current.size > 0 || settingsWaiting.current !== null;
      const base = emVoo ? POLL_FAST_MS : POLL_MS;
      // Recuo progressivo: um app esquecido aberto com o servidor fora do ar
      // não deve bater a cada poucos segundos indefinidamente (UPGRADE/06, P6).
      const falhas = pollFailures.current;
      const ms =
        falhas > 0 ? Math.min(base * Math.min(falhas, 8), POLL_BACKOFF_MAX_MS) : base;

      timer = setInterval(() => void load(), ms);
    };

    restartPolling.current = start;

    const onVisibility = () => {
      if (document.visibilityState === "visible") {
        void load();
        start();
      } else {
        stop();
      }
    };

    start();
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
        restartPolling.current(); // ritmo rápido agora, sem esperar o próximo ciclo de 4 s
        setFeedback({ id, target, status: "pending" });
        await load();
      } catch (e) {
        setFeedback({
          id: -1,
          target,
          status: "fail",
          reason: describeLoadError(e),
          hint:
            e instanceof Error && !(e instanceof ApiError && e.code === "db_unavailable")
              ? e.message
              : undefined,
        });
      }
    },
    [load],
  );

  /**
   * Dispara o autodiagnóstico e acompanha até o relatório novo chegar.
   *
   * Não usa o `feedback` dos outros comandos de propósito: aquele é um aviso
   * atrelado a uma tela de atuador ("ligar luminária"), e aqui o desfecho é o
   * próprio relatório aparecendo na lista. O que a interface precisa saber é
   * só "ainda estou esperando", para desabilitar o botão.
   */
  const runDiagnostic = useCallback(async () => {
    setDiagnosticRunning(true);
    const antes = data?.diagnostic?.ran_at ?? null;
    try {
      await sendCommand({ action: "device.diagnose" });
      restartPolling.current();
      diagnosticWaitingSince.current = { since: Date.now(), previousRanAt: antes };
    } catch {
      setDiagnosticRunning(false);
    }
  }, [data]);

  /**
   * Grava uma mudança de configuração e acompanha até o aquário aplicar.
   *
   * Manda a configuração **inteira** — o servidor valida o conjunto, porque
   * as regras cruzam campos (desliga < liga; acende ≠ apaga). `patch` é só o
   * que a tela mudou; o resto vem do que o servidor já tem.
   */
  const saveSettings = useCallback(
    async (patch: Partial<DeviceConfig>, area: SettingsArea): Promise<boolean> => {
      const atual = data?.config;
      if (!atual) {
        setSettingsFeedback({ area, status: "fail", reason: "A configuração ainda não carregou." });
        return false;
      }

      setSettingsFeedback({ area, status: "saving" });
      try {
        const versao = await saveConfig({ ...atual, ...patch });
        const aplicada = data?.state?.config_version ?? 0;

        if (versao <= aplicada) {
          // Nada mudou de fato (o servidor não sobe a versão por gravação igual).
          setSettingsFeedback({ area, status: "applied" });
        } else if (data !== null && !data.device.online) {
          setSettingsFeedback({ area, status: "queued" });
        } else {
          settingsWaiting.current = { version: versao, since: Date.now(), area };
          setSettingsFeedback({ area, status: "applying" });
          restartPolling.current();
        }
        await load();
        return true;
      } catch (e) {
        setSettingsFeedback({
          area,
          status: "fail",
          reason:
            e instanceof ApiError && e.code === "invalid_config"
              ? "O servidor recusou esses valores."
              : describeLoadError(e),
        });
        return false;
      }
    },
    [data, load],
  );

  // O aquário confirmou a versão nova (ou o prazo estourou).
  useEffect(() => {
    const espera = settingsWaiting.current;
    if (espera === null || data === null) return;

    const aplicada = data.state?.config_version ?? 0;
    if (aplicada >= espera.version) {
      settingsWaiting.current = null;
      restartPolling.current();
      setSettingsFeedback({ area: espera.area, status: "applied" });
      return;
    }
    if (!data.device.online || Date.now() - espera.since > SETTINGS_APPLY_TIMEOUT_MS) {
      settingsWaiting.current = null;
      restartPolling.current();
      setSettingsFeedback({ area: espera.area, status: "queued" });
    }
  }, [data]);

  // "Aplicado" some sozinho; os outros ficam até a próxima ação.
  useEffect(() => {
    if (settingsFeedback?.status !== "applied") return;
    const t = setTimeout(() => setSettingsFeedback(null), 4000);
    return () => clearTimeout(t);
  }, [settingsFeedback]);

  // O relatório chegou (ou o prazo estourou): libera o botão.
  useEffect(() => {
    const espera = diagnosticWaitingSince.current;
    if (espera === null) return;

    const atual = data?.diagnostic?.ran_at ?? null;
    const chegou = atual !== null && atual !== espera.previousRanAt;
    // Teto de 30 s: o dispositivo busca a fila a cada poucos segundos e o
    // autoteste leva menos de 1 s, então passar disso é que algo não voltou.
    const desistiu = Date.now() - espera.since > 30_000;

    if (chegou || desistiu) {
      diagnosticWaitingSince.current = null;
      setDiagnosticRunning(false);
    }
  }, [data]);

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
    runDiagnostic,
    diagnosticRunning,
    dismissFeedback: () => setFeedback(null),
    saveSettings,
    settingsFeedback,
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

/**
 * Traduz uma falha de carregamento para uma frase que aponta para a peça
 * certa (UPGRADE/06, P3). Antes, qualquer erro — rede fora do ar, servidor de
 * pé mas sem banco — virava a mesma frase genérica de rede, e "Servidor sem
 * banco de dados" (uma das quatro frases previstas no desenho de
 * observabilidade) nunca aparecia: o servidor respondia, só que com `503` e
 * `{"error":"db_unavailable"}`, e isso caía no mesmo balde de "sem contato".
 */
function describeLoadError(e: unknown): string {
  if (e instanceof ApiError && e.code === "db_unavailable") {
    return "Servidor sem banco de dados";
  }
  return e instanceof Error ? e.message : "falha ao carregar";
}
