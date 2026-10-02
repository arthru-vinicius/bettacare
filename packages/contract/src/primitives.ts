import { z } from "zod";

/**
 * Vocabulário compartilhado por todo o resto do contrato.
 * Nada aqui depende de banco, de HTTP ou de React.
 */

/** Componentes de que o sistema sabe falar. */
export const COMPONENTS = [
  // infraestrutura
  "system",
  "wifi",
  "api",
  // hardware do aquário
  "rtc",
  "temp",
  "fan",
  "light",
  "button",
  "pot",
  // módulo opcional de alimentação — fora de AQUARIUM_COMPONENTS de propósito,
  // ver o comentário em `evaluateHealth` (health/evaluate.ts): desconectado é
  // estado normal deste módulo, não falha, e não deve influenciar o veredito
  // geral do aquário.
  "feeder",
  // armazenamento e manutenção do próprio dispositivo
  "nvs",
  "ota",
] as const;

export const componentSchema = z.enum(COMPONENTS);
export type Component = z.infer<typeof componentSchema>;

/**
 * Só estes aparecem na aba Saúde; os demais são diagnóstico de bastidor.
 *
 * `button` e `pot` entraram na rodada de confiabilidade de 2026-08-21: são
 * entradas físicas que também falham, e um potenciômetro com mau contato era
 * exatamente o exemplo citado como "hoje invisível" — ver UPGRADE/04 (S2).
 */
export const AQUARIUM_COMPONENTS = [
  "temp",
  "fan",
  "light",
  "rtc",
  "wifi",
  "button",
  "pot",
] as const satisfies readonly Component[];

export const SEVERITIES = ["debug", "info", "warn", "error", "fatal"] as const;
export const severitySchema = z.enum(SEVERITIES);
export type Severity = z.infer<typeof severitySchema>;

/** Ordem de gravidade — usada para filtrar "erros e acima" na interface. */
export const SEVERITY_RANK: Record<Severity, number> = {
  debug: 10,
  info: 20,
  warn: 30,
  error: 40,
  fatal: 50,
};

/**
 * Estado de saúde de um componente.
 *
 * - `ok`       funcionando dentro do esperado
 * - `degraded` responde, mas fora do ideal (RSSI baixo, leitura velha)
 * - `missing`  não foi encontrado no barramento — deixou de ser reconhecido
 * - `fault`    presente, porém com evidência concreta de defeito
 * - `unknown`  ainda sem informação (boot recente, dispositivo offline)
 */
export const HEALTH_STATUSES = [
  "ok",
  "degraded",
  "missing",
  "fault",
  "unknown",
] as const;

export const healthStatusSchema = z.enum(HEALTH_STATUSES);
export type HealthStatus = z.infer<typeof healthStatusSchema>;

/**
 * Código de evento: `componente.motivo`, em minúsculas.
 *
 * Deliberadamente **não** é um enum fechado. Um firmware mais novo pode emitir
 * um código que este servidor ainda não conhece, e isso não pode derrubar a
 * ingestão inteira — o evento é gravado e a interface cai no rótulo genérico.
 * O catálogo em `codes.ts` existe só para traduzir os conhecidos.
 */
export const eventCodeSchema = z
  .string()
  .min(3)
  .max(48)
  .regex(
    /^[a-z][a-z0-9]*\.[a-z0-9_]+$/,
    "código deve ter o formato 'componente.motivo'",
  );

/**
 * Tetos numéricos — o contrato precisa conhecer o limite do banco.
 *
 * Um valor que o Zod aceita e a coluna recusa vira exceção no meio da
 * transação e **HTTP 500**: o dispositivo entende "o servidor quebrou" e
 * reenvia para sempre, quando a resposta honesta seria 400, "seu corpo é
 * inválido". Aconteceu de verdade — o firmware mandava `UINT32_MAX` como
 * sentinela de `age_ms`, `z.int().min(0)` deixava passar, e a coluna `integer`
 * estourava a cada POST.
 *
 * A regra que fica: todo inteiro do contrato declara um teto, e o teto é o da
 * coluna que o recebe.
 */
export const INT32_MAX = 2_147_483_647;
/** Tudo que vem de `millis()` ou de contador `uint32_t` no ESP32. */
export const UINT32_MAX = 4_294_967_295;

/** Identificador do dispositivo — slug estável, gravado no NVS. */
export const deviceIdSchema = z
  .string()
  .min(1)
  .max(64)
  .regex(/^[a-z0-9][a-z0-9-]*$/, "device_id deve ser um slug em minúsculas");

/** Horário no formato "HH:MM", 24h. */
export const timeOfDaySchema = z
  .string()
  .regex(/^([01]\d|2[0-3]):[0-5]\d$/, "esperado HH:MM em 24 horas");

/** Instante em UTC. Sempre `timestamptz` do lado do banco. */
export const isoInstantSchema = z.iso.datetime({ offset: true });
