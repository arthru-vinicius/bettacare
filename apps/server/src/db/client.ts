import { existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { drizzle, type NodePgDatabase } from "drizzle-orm/node-postgres";
import { migrate } from "drizzle-orm/node-postgres/migrator";
import { sql } from "drizzle-orm";
import pg from "pg";

import * as schema from "./schema.js";

export type Db = NodePgDatabase<typeof schema>;

/**
 * Localiza a pasta de migrations subindo a árvore a partir deste módulo.
 *
 * Um caminho relativo fixo quebraria: o mesmo código roda a partir de `dist/`
 * em produção e de `dist-test/src/` nos testes, que têm profundidades
 * diferentes. Procurar o `_journal.json` funciona nos dois, e falha alto na
 * subida em vez de silenciosamente não aplicar migration nenhuma.
 */
function findMigrationsFolder(): string {
  let dir = dirname(fileURLToPath(import.meta.url));

  for (let i = 0; i < 6; i++) {
    const candidate = join(dir, "drizzle");
    if (existsSync(join(candidate, "meta", "_journal.json"))) return candidate;
    dir = dirname(dir);
  }

  throw new Error(
    "pasta de migrations não encontrada — o build não incluiu drizzle/?",
  );
}

export const MIGRATIONS_FOLDER: string = findMigrationsFolder();

export function createPool(url: string, max: number): pg.Pool {
  return new pg.Pool({
    connectionString: url,
    max,
    // Sem isso, uma conexão pendurada trava a subida do processo indefinidamente.
    connectionTimeoutMillis: 5_000,
    idleTimeoutMillis: 30_000,
    // O banco é sempre UTC; o fuso de apresentação nunca sai de TZ_DISPLAY.
    options: "-c timezone=UTC",
  });
}

export function createDb(pool: pg.Pool): Db {
  return drizzle(pool, { schema });
}

/**
 * Aplica as migrations pendentes.
 *
 * Idempotente: o migrator mantém a tabela `__drizzle_migrations` e aplica só o
 * que falta. O playbook do homelab roda o mesmo compose várias vezes, então
 * isso não é um detalhe — é requisito.
 */
export async function runMigrations(db: Db): Promise<void> {
  await migrate(db, { migrationsFolder: MIGRATIONS_FOLDER });
}

/**
 * Garante a partição do mês corrente e a do mês seguinte.
 *
 * Roda na subida **e** no job diário. Um INSERT sem partição correspondente
 * falha, e a virada do mês acontece às 00:00 — não é hora de descobrir isso.
 */
export async function ensureCurrentPartitions(db: Db): Promise<void> {
  await db.execute(sql`select bettacare_ensure_current_partitions()`);
}

/** Tamanho atual do database `bettacare`, em bytes. */
export async function databaseSizeBytes(db: Db): Promise<number> {
  const r = await db.execute<{ size: string }>(
    sql`select pg_database_size(current_database())::text as size`,
  );
  return Number(r.rows[0]?.size ?? 0);
}

/**
 * Derruba as partições mensais mais antigas até o banco caber no teto.
 * Devolve o que foi descartado, para virar evento `db.partition_dropped`.
 */
export async function enforceSizeLimit(
  db: Db,
  limitBytes: number,
): Promise<Array<{ partition: string; freedBytes: number }>> {
  const r = await db.execute<{
    dropped_partition: string;
    freed_bytes: string;
  }>(sql`select * from bettacare_enforce_size_limit(${limitBytes}::bigint)`);

  return r.rows.map((row) => ({
    partition: row.dropped_partition,
    freedBytes: Number(row.freed_bytes),
  }));
}

/** `SELECT 1` com timeout curto — é tudo que o `/healthz` precisa fazer. */
export async function pingDatabase(pool: pg.Pool): Promise<boolean> {
  const client = await pool.connect();
  try {
    await client.query("set local statement_timeout = 2000");
    await client.query("select 1");
    return true;
  } catch {
    return false;
  } finally {
    client.release();
  }
}
