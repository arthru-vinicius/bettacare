import type pg from "pg";

import type { AppConfig } from "./config.js";
import type { Db } from "./db/client.js";
import type { Logger } from "./logger.js";

/**
 * O que os roteadores precisam ter em mãos.
 *
 * `dbReady` existe porque o contrato do homelab exige que a aplicação **suba
 * mesmo com o PostgreSQL indisponível**: depois de um reboot do servidor todos
 * os containers sobem juntos e não há garantia de ordem. Os listeners HTTP
 * abrem primeiro; a conexão e as migrations acontecem em segundo plano, com
 * backoff. Enquanto isso não termina, `/healthz` responde 503 e o ingest
 * responde 503 com corpo curto — e o aquário segue operando sozinho.
 */
export interface Runtime {
  cfg: AppConfig;
  db: Db;
  pool: pg.Pool;
  log: Logger;
  isDbReady(): boolean;
  setDbReady(ready: boolean): void;
}

export function createRuntime(parts: {
  cfg: AppConfig;
  db: Db;
  pool: pg.Pool;
  log: Logger;
}): Runtime {
  let ready = false;
  return {
    ...parts,
    isDbReady: () => ready,
    setDbReady: (value: boolean) => {
      ready = value;
    },
  };
}
