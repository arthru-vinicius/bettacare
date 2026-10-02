import { z } from "zod";
import {
  componentSchema,
  healthStatusSchema,
  isoInstantSchema,
} from "./primitives.js";

/**
 * Enquanto `events` guarda *o que aconteceu*, isto guarda *como está agora*.
 * Uma linha por (dispositivo, componente), gravada só quando o status muda —
 * é uma tabela minúscula que nunca cresce e alimenta a aba Saúde sem varrer
 * histórico nenhum.
 */
export const componentHealthSchema = z.object({
  comp: componentSchema,
  status: healthStatusSchema,
  /** Desde quando está neste status. É o "sumiu às 14:02". */
  since: isoInstantSchema,
  /** Último instante em que esteve `ok`. Nulo se nunca esteve. */
  last_ok_at: isoInstantSchema.nullable(),
  /** O código do evento que causou a transição atual. */
  last_code: z.string().nullable(),
  /** Valor corrente relevante do componente: `{ celsius: 26.5 }`, `{ rpm: 1380 }`. */
  detail: z.record(z.string(), z.unknown()).nullable(),
});
export type ComponentHealth = z.infer<typeof componentHealthSchema>;

/**
 * A visão de saúde que o **firmware** manda no POST. É a opinião do dispositivo
 * sobre si mesmo — o servidor combina com a própria (que enxerga coisas que o
 * firmware não vê, como "parou de fazer POST").
 */
export const deviceHealthReportSchema = z.partialRecord(
  componentSchema,
  healthStatusSchema,
);
export type DeviceHealthReport = z.infer<typeof deviceHealthReportSchema>;

/** Saúde agregada do aquário — o que o cabeçalho do app mostra. */
export const OVERALL_STATUSES = ["ok", "attention", "critical", "offline"] as const;
export const overallStatusSchema = z.enum(OVERALL_STATUSES);
export type OverallStatus = z.infer<typeof overallStatusSchema>;

/**
 * Reduz o conjunto de componentes a um veredito só.
 *
 * `offline` vence tudo: se o dispositivo não fala, qualquer outra informação
 * é velha e afirmar "tudo ok" com base nela seria mentira.
 */
export function summarizeHealth(
  components: readonly ComponentHealth[],
  isOnline: boolean,
): OverallStatus {
  if (!isOnline) return "offline";
  if (components.some((c) => c.status === "fault" || c.status === "missing")) {
    return "critical";
  }
  if (components.some((c) => c.status === "degraded")) return "attention";
  return "ok";
}

export const HEALTH_STATUS_LABELS: Record<
  z.infer<typeof healthStatusSchema>,
  string
> = {
  ok: "Normal",
  degraded: "Instável",
  missing: "Não reconhecido",
  fault: "Em falha",
  unknown: "Sem informação",
};

export const COMPONENT_LABELS: Record<z.infer<typeof componentSchema>, string> = {
  system: "Sistema",
  wifi: "Wi-Fi",
  api: "Comunicação",
  rtc: "Relógio (DS3231)",
  temp: "Temperatura (DS18B20)",
  fan: "Ventoinha",
  light: "Luminária",
  button: "Botão físico",
  pot: "Potenciômetro",
  feeder: "Alimentador",
  nvs: "Memória de configuração",
  ota: "Atualização remota",
};

/**
 * Silêncio a partir do qual o dispositivo é considerado offline. Vinte vezes o
 * intervalo padrão de POST — tolera uma reconexão de Wi-Fi sem alarme falso.
 */
export const DEFAULT_OFFLINE_AFTER_S = 60;
