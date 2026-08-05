import { z } from "zod";
import {
  componentSchema,
  eventCodeSchema,
  isoInstantSchema,
  severitySchema,
} from "./primitives.js";

/**
 * Um evento é o registro de *algo que aconteceu*. O estado corrente vive em
 * outro lugar (`health.ts`) — misturar os dois foi o que tornou o log antigo
 * inútil para diagnóstico.
 */

/** Quem gerou o evento. Importa: quando o ESP32 está mudo, quem fala é o servidor. */
export const eventSourceSchema = z.enum(["device", "server"]);
export type EventSource = z.infer<typeof eventSourceSchema>;

/**
 * Contexto opcional do evento. Mantido raso e pequeno de propósito — ele
 * trafega no POST do ESP32, que tem memória escassa.
 */
export const eventContextSchema = z.record(
  z.string().max(24),
  z.union([z.string().max(120), z.number(), z.boolean(), z.null()]),
);

/** Evento como o firmware o envia, dentro do POST de telemetria. */
export const deviceEventSchema = z.object({
  /** Relógio do dispositivo. Pode divergir: o servidor carimba o dele também. */
  t: isoInstantSchema.optional(),
  sev: severitySchema,
  comp: componentSchema,
  code: eventCodeSchema,
  msg: z.string().max(240),
  ctx: eventContextSchema.optional(),
  /**
   * Quantas vezes o mesmo (comp, code) se repetiu antes de ser drenado.
   * O firmware colapsa repetições em vez de mandar N linhas iguais — um
   * sensor oscilando não pode inundar o banco.
   */
  repeat_count: z.int().min(1).max(65535).default(1),
});
export type DeviceEvent = z.infer<typeof deviceEventSchema>;

/** Evento já persistido, como a interface o recebe. */
export const storedEventSchema = deviceEventSchema.extend({
  id: z.string(),
  device_id: z.string(),
  source: eventSourceSchema,
  /** Carimbado pelo servidor — este é o horário autoritativo. */
  received_at: isoInstantSchema,
  t: isoInstantSchema.nullable(),
});
export type StoredEvent = z.infer<typeof storedEventSchema>;

/**
 * Teto de eventos por POST. O firmware acumula entre envios e drena aqui;
 * o que passar disso fica para o POST seguinte, com um evento
 * `system.event_overflow` sinalizando a perda.
 */
export const MAX_EVENTS_PER_POST = 24;
