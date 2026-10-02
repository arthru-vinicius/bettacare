import { serve } from "@hono/node-server";

import { createApiRouter } from "./api/router.js";
import { loadConfig } from "./config.js";
import {
  createDb,
  createPool,
  ensureCurrentPartitions,
  runMigrations,
} from "./db/client.js";
import { createIngestRouter } from "./ingest/router.js";
import { startMaintenance } from "./jobs/maintenance.js";
import { startWatchdog } from "./jobs/watchdog.js";
import { createLogger } from "./logger.js";
import { ensurePushKeys } from "./push/notify.js";
import { createRuntime } from "./runtime.js";
import { mountStatic, resolveWebRoot, webBuildExists } from "./static.js";

const cfg = loadConfig();
const log = createLogger(cfg);
const pool = createPool(cfg.DATABASE_URL, cfg.PG_POOL_MAX);
const db = createDb(pool);
const rt = createRuntime({ cfg, db, pool, log });

// ── Os dois roteadores ──────────────────────────────────────────────────────
// Portas separadas, públicos com exigências incompatíveis. Uma requisição que
// chega na 8080 não alcança rota nenhuma da UI, e vice-versa.

const webRoot = resolveWebRoot(cfg.WEB_ROOT);

const uiApp = createApiRouter(rt);
mountStatic(uiApp, webRoot);

const ingestApp = createIngestRouter(rt);

if (!webBuildExists(webRoot)) {
  log.warn({ webRoot }, "export do Next não encontrado — a UI responderá 503");
}

// ── Os listeners abrem antes do banco ───────────────────────────────────────
// Exigência do contrato: depois de um reboot do servidor todos os containers
// sobem juntos e não há garantia de ordem. Quem não sobe sem o Postgres entra
// em laço de reinício.

const uiServer = serve({ fetch: uiApp.fetch, port: cfg.PORT_UI, hostname: "0.0.0.0" });
const ingestServer = serve({
  fetch: ingestApp.fetch,
  port: cfg.PORT_INGEST,
  hostname: "0.0.0.0",
});

log.info(
  { ui: cfg.PORT_UI, ingest: cfg.PORT_INGEST, env: cfg.NODE_ENV },
  "bettacare no ar",
);

// ── Banco em segundo plano, com backoff ─────────────────────────────────────

let stopWatchdog: (() => void) | undefined;
let stopMaintenance: (() => void) | undefined;

async function connectWithBackoff(): Promise<void> {
  let delay = 1000;

  for (;;) {
    try {
      await runMigrations(db);
      await ensureCurrentPartitions(db);
      rt.setDbReady(true);

      // Depende do banco: o par VAPID é persistido em `server_keys` para
      // sobreviver a um deploy sem invalidar as inscrições existentes.
      try {
        rt.setVapidPublicKey(await ensurePushKeys(rt));
      } catch (err) {
        // Push é um recurso adicional: sem ele o sistema inteiro continua
        // funcionando, então uma falha aqui não pode impedir a subida.
        log.error({ err }, "não foi possível inicializar as notificações push");
      }

      stopWatchdog = startWatchdog(rt);
      stopMaintenance = startMaintenance(rt);

      log.info("banco pronto, migrations aplicadas, jobs iniciados");
      return;
    } catch (err) {
      rt.setDbReady(false);
      log.error({ err, retry_ms: delay }, "banco indisponível, tentando de novo");
      await new Promise((r) => setTimeout(r, delay));
      // Teto de 30 s: adianta pouco esperar mais do que isso, e um teto baixo
      // faz o serviço voltar rápido quando o Postgres enfim sobe.
      delay = Math.min(delay * 2, 30_000);
    }
  }
}

void connectWithBackoff();

// ── Encerramento ────────────────────────────────────────────────────────────
// Sem tratar SIGTERM, todo `docker compose down` espera dez segundos e mata o
// processo à força. Como o Node é o PID 1 e trata o sinal, não é preciso
// `init: true` nem `tini` no compose.

let shuttingDown = false;

async function shutdown(signal: string): Promise<void> {
  if (shuttingDown) return;
  shuttingDown = true;

  log.info({ signal }, "encerrando");
  stopWatchdog?.();
  stopMaintenance?.();

  const fecharListeners = Promise.all([
    new Promise<void>((r) => uiServer.close(() => r())),
    new Promise<void>((r) => ingestServer.close(() => r())),
  ]);

  // Aguarda as requisições em voo, com teto — uma conexão pendurada não pode
  // segurar o desligamento indefinidamente.
  await Promise.race([
    fecharListeners,
    new Promise((r) => setTimeout(r, 5000)),
  ]);

  await pool.end().catch(() => undefined);
  log.info("encerrado");
  process.exit(0);
}

process.on("SIGTERM", () => void shutdown("SIGTERM"));
process.on("SIGINT", () => void shutdown("SIGINT"));

process.on("unhandledRejection", (err) => {
  log.error({ err }, "promessa rejeitada sem tratamento");
});
