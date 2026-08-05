import { ACCESS_EMAIL_HEADER } from "@bettacare/contract";
import type { Context, MiddlewareHandler } from "hono";

import type { AppConfig } from "../config.js";

/**
 * Identidade vinda do Cloudflare Access.
 *
 * A aplicação **não implementa login próprio**: requisição não autenticada é
 * barrada na borda e nunca toca a máquina. O que chega aqui já passou pelo
 * Access, e o e-mail serve para atribuir autoria — dá para saber quem ligou a
 * luz e quem mudou o horário.
 *
 * Este middleware existe **apenas na porta da interface**. Na porta do ESP32
 * o cabeçalho é ignorado incondicionalmente, mesmo se presente: aquele tráfego
 * vem da LAN, não passa pelo Access, e confiar nele ali seria confiar em
 * qualquer aparelho da rede.
 */
declare module "hono" {
  interface ContextVariableMap {
    userEmail: string | null;
  }
}

export function accessIdentity(cfg: AppConfig): MiddlewareHandler {
  return async (c, next) => {
    const raw = c.req.header(ACCESS_EMAIL_HEADER)?.trim().toLowerCase() ?? "";
    const email = raw.length > 0 ? raw : null;

    // Defesa em profundidade opcional: o Access já barra quem não deve entrar,
    // e esta lista só existe para o caso de a policy ser afrouxada por engano.
    // Vazia, a aplicação confia inteiramente no Access.
    if (
      cfg.ALLOWED_USER_EMAILS.length > 0 &&
      (email === null || !cfg.ALLOWED_USER_EMAILS.includes(email))
    ) {
      return c.json({ error: "forbidden" }, 403);
    }

    c.set("userEmail", email);
    await next();
  };
}

export function currentUser(c: Context): string | null {
  return c.get("userEmail") ?? null;
}
