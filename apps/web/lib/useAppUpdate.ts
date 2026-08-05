"use client";

import { useCallback, useEffect, useRef, useState } from "react";

/**
 * Detecção e aplicação de atualização do PWA.
 *
 * O problema clássico: o service worker serve a versão em cache, o usuário
 * fica semanas numa build velha e nem sabe. A defesa tem três camadas, e as
 * três precisam existir:
 *
 * 1. **Cabeçalhos** — `sw.js`, `version.json` e o HTML são `no-store`,
 *    definidos pelo Hono em `apps/server/src/static.ts`. Um service worker em
 *    cache é um service worker que nunca se atualiza.
 * 2. **Carimbo de versão** — `/version.json` carrega o `buildId` da build que
 *    está no servidor. O app compara com o seu.
 * 3. **Atualização forçada** — se o build corrente for anterior ao `minBuild`
 *    declarado pela build nova, recarrega sozinho, sem perguntar.
 */

const CHECK_INTERVAL_MS = 15 * 60 * 1000;

interface VersionInfo {
  buildId: string;
  version: string;
  /** Builds anteriores a este são recarregadas sem perguntar. */
  minBuild: string | null;
  builtAt: string;
}

export interface UpdateState {
  /** Uma versão nova está disponível e aguarda o toque do usuário. */
  available: boolean;
  /** Build que este cliente está rodando. */
  current: string | null;
  apply: () => void;
}

export function useAppUpdate(): UpdateState {
  const [available, setAvailable] = useState(false);
  const [current, setCurrent] = useState<string | null>(null);

  const myBuild = useRef<string | null>(null);
  /** Trava contra laço de recarga: `controllerchange` pode disparar mais de uma vez. */
  const reloading = useRef(false);

  const applyNow = useCallback(() => {
    if (reloading.current) return;
    reloading.current = true;

    void navigator.serviceWorker?.getRegistration().then((reg) => {
      if (reg?.waiting) {
        reg.waiting.postMessage("SKIP_WAITING");
        // O `controllerchange` recarrega. Se não houver worker em espera (por
        // exemplo, service worker desabilitado no navegador), o fallback
        // abaixo garante que a atualização aconteça mesmo assim.
        setTimeout(() => window.location.reload(), 1200);
      } else {
        window.location.reload();
      }
    });
  }, []);

  useEffect(() => {
    let cancelado = false;

    async function checar() {
      try {
        // `cache: no-store` no fetch além do cabeçalho: cinto e suspensório,
        // porque é esta requisição que enxerga a versão nova.
        const r = await fetch("/version.json", { cache: "no-store" });
        if (!r.ok) return;
        const info = (await r.json()) as VersionInfo;
        if (cancelado) return;

        if (myBuild.current === null) {
          myBuild.current = info.buildId;
          setCurrent(info.buildId);
          return;
        }

        if (info.buildId === myBuild.current) return;

        // Build nova detectada. Pede ao navegador para reavaliar o sw.js —
        // sem isto, o worker em espera pode demorar horas para aparecer.
        const reg = await navigator.serviceWorker?.getRegistration();
        await reg?.update();

        if (info.minBuild !== null && myBuild.current < info.minBuild) {
          // Versão quebrada no ar ou contrato de API incompatível: não há o
          // que perguntar ao usuário.
          applyNow();
          return;
        }

        setAvailable(true);
      } catch {
        // Sem rede: nada a fazer, e o app segue funcionando do cache.
      }
    }

    void checar();
    const timer = setInterval(() => void checar(), CHECK_INTERVAL_MS);

    const onFocus = () => {
      if (document.visibilityState === "visible") void checar();
    };
    document.addEventListener("visibilitychange", onFocus);

    return () => {
      cancelado = true;
      clearInterval(timer);
      document.removeEventListener("visibilitychange", onFocus);
    };
  }, [applyNow]);

  // Registro do worker e a recarga quando ele assume.
  useEffect(() => {
    if (!("serviceWorker" in navigator)) return;

    const onControllerChange = () => {
      if (reloading.current) return;
      reloading.current = true;
      window.location.reload();
    };

    navigator.serviceWorker.addEventListener(
      "controllerchange",
      onControllerChange,
    );

    void navigator.serviceWorker.register("/sw.js").catch(() => undefined);

    return () => {
      navigator.serviceWorker.removeEventListener(
        "controllerchange",
        onControllerChange,
      );
    };
  }, []);

  return { available, current, apply: applyNow };
}
