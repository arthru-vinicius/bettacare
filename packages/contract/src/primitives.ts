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
  // armazenamento e manutenção do próprio dispositivo
  "nvs",
  "ota",
] as const;

export const componentSchema = z.enum(COMPONENTS);
export type Component = z.infer<typeof componentSchema>;

/** Só estes aparecem na aba Saúde; os demais são diagnóstico de bastidor. */
export const AQUARIUM_COMPONENTS = [
  "temp",
  "fan",
  "light",
  "rtc",
  "wifi",
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
