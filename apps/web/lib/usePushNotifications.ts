"use client";

import { useCallback, useEffect, useState } from "react";

/**
 * Notificações push do navegador.
 *
 * Só faz sentido pedir permissão quando o usuário **entende o que vai
 * receber** — por isso a permissão não é pedida na abertura do app, e sim num
 * botão explícito na aba Diagnóstico. Um pedido de permissão que aparece antes
 * de o usuário saber o que é costuma ser negado, e uma vez negado o navegador
 * não pergunta de novo: uma decisão precipitada custa o recurso para sempre.
 */

export type PushState =
  | "carregando"
  /** O navegador não suporta (iOS fora da tela de início, por exemplo). */
  | "indisponivel"
  /** Suporta, mas o servidor não tem chaves configuradas. */
  | "desabilitado"
  | "negado"
  | "inativo"
  | "ativo";

interface PushApi {
  state: PushState;
  ativar: () => Promise<void>;
  desativar: () => Promise<void>;
  testar: () => Promise<void>;
  erro: string | null;
}

/** A chave VAPID viaja em base64url e a API do navegador quer bytes crus. */
function urlBase64ToUint8Array(base64: string): Uint8Array {
  const padding = "=".repeat((4 - (base64.length % 4)) % 4);
  const normal = (base64 + padding).replace(/-/g, "+").replace(/_/g, "/");
  const raw = atob(normal);
  const out = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i++) out[i] = raw.charCodeAt(i);
  return out;
}

export function usePushNotifications(): PushApi {
  const [state, setState] = useState<PushState>("carregando");
  const [erro, setErro] = useState<string | null>(null);

  useEffect(() => {
    let cancelado = false;

    void (async () => {
      if (
        typeof window === "undefined" ||
        !("serviceWorker" in navigator) ||
        !("PushManager" in window) ||
        !("Notification" in window)
      ) {
        if (!cancelado) setState("indisponivel");
        return;
      }

      try {
        const r = await fetch("/api/push/key", { cache: "no-store" });
        const { enabled } = (await r.json()) as { enabled: boolean };
        if (!enabled) {
          if (!cancelado) setState("desabilitado");
          return;
        }
      } catch {
        if (!cancelado) setState("desabilitado");
        return;
      }

      if (Notification.permission === "denied") {
        if (!cancelado) setState("negado");
        return;
      }

      const reg = await navigator.serviceWorker.ready;
      const sub = await reg.pushManager.getSubscription();
      if (!cancelado) setState(sub ? "ativo" : "inativo");
    })();

    return () => {
      cancelado = true;
    };
  }, []);

  const ativar = useCallback(async () => {
    setErro(null);
    try {
      const permissao = await Notification.requestPermission();
      if (permissao !== "granted") {
        setState(permissao === "denied" ? "negado" : "inativo");
        return;
      }

      const r = await fetch("/api/push/key", { cache: "no-store" });
      const { key } = (await r.json()) as { key: string | null };
      if (!key) {
        setState("desabilitado");
        return;
      }

      const reg = await navigator.serviceWorker.ready;
      const sub = await reg.pushManager.subscribe({
        // Exigido pelos navegadores desde o Chrome 52: toda notificação
        // precisa ser visível ao usuário. Não dá para usar push silencioso.
        userVisibleOnly: true,
        applicationServerKey: urlBase64ToUint8Array(key) as BufferSource,
      });

      const resp = await fetch("/api/push/subscribe", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify(sub.toJSON()),
      });
      if (!resp.ok) throw new Error(`servidor recusou a inscrição (${resp.status})`);

      setState("ativo");
    } catch (e) {
      setErro(e instanceof Error ? e.message : "não foi possível ativar");
      setState("inativo");
    }
  }, []);

  const desativar = useCallback(async () => {
    setErro(null);
    try {
      const reg = await navigator.serviceWorker.ready;
      const sub = await reg.pushManager.getSubscription();
      if (sub) {
        // Avisa o servidor **antes** de cancelar localmente: se a ordem fosse
        // inversa e a rede falhasse, o servidor continuaria mandando push para
        // um endpoint que já não existe.
        await fetch("/api/push/subscribe", {
          method: "DELETE",
          headers: { "content-type": "application/json" },
          body: JSON.stringify({ endpoint: sub.endpoint }),
        }).catch(() => undefined);
        await sub.unsubscribe();
      }
      setState("inativo");
    } catch (e) {
      setErro(e instanceof Error ? e.message : "não foi possível desativar");
    }
  }, []);

  const testar = useCallback(async () => {
    setErro(null);
    try {
      await fetch("/api/push/test", { method: "POST" });
    } catch (e) {
      setErro(e instanceof Error ? e.message : "falha ao enviar teste");
    }
  }, []);

  return { state, ativar, desativar, testar, erro };
}
