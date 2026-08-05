import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { serveStatic } from "@hono/node-server/serve-static";
import type { Hono } from "hono";

/**
 * Serve o export estático do Next.
 *
 * Os cabeçalhos de cache são o ponto onde a briga da atualização do PWA é
 * ganha — e como o Hono serve os arquivos, eles são nossos: não dependemos de
 * configuração de CDN nem de proxy, que aliás estão proibidos no stack.
 *
 * A regra é curta:
 *
 * - `/_next/static/**` tem hash no nome, então nunca muda de conteúdo:
 *   cache eterno e imutável.
 * - `sw.js`, `version.json` e todo HTML são `no-store`. O service worker é o
 *   crítico: um service worker em cache é um service worker que nunca se
 *   atualiza, e o app fica preso numa build velha para sempre.
 */
/** Caminho de desenvolvimento, relativo a `dist/`. Em produção vem de `WEB_ROOT`. */
const DEV_WEB_ROOT = fileURLToPath(new URL("../../web/out", import.meta.url));

export function resolveWebRoot(configured: string): string {
  return configured.length > 0 ? configured : DEV_WEB_ROOT;
}

const NEVER_CACHE = /^\/(sw\.js|version\.json|manifest\.webmanifest)$|\.html?$/;
const IMMUTABLE = /^\/_next\/static\//;

export function mountStatic(app: Hono, root: string): void {
  app.use("*", async (c, next) => {
    await next();

    // Só arquivos servidos daqui; respostas de API têm as próprias regras.
    if (c.req.path.startsWith("/api/")) return;

    const path = c.req.path;
    if (IMMUTABLE.test(path)) {
      c.header("Cache-Control", "public, max-age=31536000, immutable");
    } else if (NEVER_CACHE.test(path) || path === "/") {
      c.header("Cache-Control", "no-store, must-revalidate");
    } else {
      // Ícones e afins: revalida sempre, mas aceita 304.
      c.header("Cache-Control", "public, max-age=0, must-revalidate");
    }

    c.header("X-Content-Type-Options", "nosniff");
    c.header("Referrer-Policy", "strict-origin-when-cross-origin");
  });

  app.use(
    "*",
    serveStatic({
      root,
      // A interface é uma SPA exportada: qualquer rota desconhecida cai no
      // index e o roteador do lado do cliente resolve.
      rewriteRequestPath: (p) => (p === "/" ? "/index.html" : p),
    }),
  );

  app.get("*", async (c) => {
    if (c.req.path.startsWith("/api/")) return c.json({ error: "not_found" }, 404);
    return c.text("Interface ainda não construída — rode `pnpm build`.", 503);
  });
}

/** Avisa cedo, na subida, em vez de deixar o usuário achar uma tela em branco. */
export function webBuildExists(root: string): boolean {
  return existsSync(root);
}
