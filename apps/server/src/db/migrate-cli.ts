/**
 * Aplicação manual das migrations, para o caso de precisar rodar fora do ciclo
 * de subida do container. O caminho normal é automático, em `main.ts`.
 *
 *   pnpm --filter @bettacare/server db:migrate
 */
import { loadConfig } from "../config.js";
import {
  createDb,
  createPool,
  databaseSizeBytes,
  ensureCurrentPartitions,
  runMigrations,
} from "./client.js";

const cfg = loadConfig({
  // Este CLI não precisa de token de ingestão; um placeholder satisfaz o schema.
  DEVICE_INGEST_TOKEN: "cli-nao-usa-este-token",
  ...process.env,
});

const pool = createPool(cfg.DATABASE_URL, 1);
const db = createDb(pool);

try {
  await runMigrations(db);
  await ensureCurrentPartitions(db);
  const bytes = await databaseSizeBytes(db);
  console.log(
    `migrations aplicadas — banco com ${(bytes / 1024 / 1024).toFixed(1)} MB ` +
      `(teto ${(cfg.DB_SIZE_LIMIT_BYTES / 1024 / 1024).toFixed(0)} MB)`,
  );
} finally {
  await pool.end();
}
