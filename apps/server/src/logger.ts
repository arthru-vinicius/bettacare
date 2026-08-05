import { pino } from "pino";

import type { AppConfig } from "./config.js";

/**
 * Uma linha JSON por evento, em stdout/stderr.
 *
 * Nada é escrito em arquivo dentro do container: o Docker está com driver
 * `json-file`, teto de 10 MB e três arquivos por container, e log em arquivo
 * ali dentro é perdido no primeiro `docker compose down` e não rotaciona.
 */
export function createLogger(cfg: AppConfig) {
  return pino({
    level: cfg.LOG_LEVEL,
    base: { app: "bettacare" },
    // UTC, como todo o resto. O fuso só aparece na apresentação.
    timestamp: pino.stdTimeFunctions.isoTime,
    redact: {
      paths: [
        "req.headers.x-api-token",
        "req.headers.authorization",
        "req.headers.cookie",
      ],
      censor: "[oculto]",
    },
    ...(cfg.NODE_ENV === "development"
      ? {
          transport: {
            target: "pino/file",
            options: { destination: 1 },
          },
        }
      : {}),
  });
}

export type Logger = ReturnType<typeof createLogger>;
