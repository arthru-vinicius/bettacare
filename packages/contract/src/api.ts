import { z } from "zod";
import { commandStatusSchema, commandTargetSchema } from "./commands.js";
import { deviceConfigSchema } from "./config.js";
import { eventSourceSchema } from "./events.js";
import { componentHealthSchema, overallStatusSchema } from "./health.js";
import {
  componentSchema,
  eventCodeSchema,
  healthStatusSchema,
  isoInstantSchema,
  severitySchema,
  timeOfDaySchema,
} from "./primitives.js";
import { diagnosticCheckSchema, feederStateSchema } from "./telemetry.js";

/** Relatório de autodiagnóstico já persistido, como a interface o recebe. */
export const diagnosticResultSchema = z.object({
  ran_at: isoInstantSchema,
  command_id: z.int().nullable(),
  duration_ms: z.int(),
  /** Pior status entre os checks — o veredito de uma linha só. */
  overall: healthStatusSchema,
  checks: z.array(diagnosticCheckSchema),
});
export type DiagnosticResult = z.infer<typeof diagnosticResultSchema>;

/**
 * A superfície que o navegador consome.
 *
 * Fica aqui pela mesma razão que o contrato do ESP32: no sistema antigo o
 * formato do JSON estava escrito à mão em três lugares e os três já divergiam.
 * Ter o servidor e a interface tipando pelo mesmo módulo elimina a classe
 * inteira desse bug.
 */

export const overviewResponseSchema = z.object({
  device: z.object({
    device_id: z.string(),
    name: z.string(),
    fw_version: z.string().nullable(),
    online: z.boolean(),
    last_seen_at: isoInstantSchema.nullable(),
    last_ip: z.string().nullable(),
  }),
  overall: overallStatusSchema,
  components: z.array(componentHealthSchema),
  state: z
    .object({
      light: z.object({
        on: z.boolean(),
        source: z.string(),
        desired: z.boolean().nullable(),
      }),
      temperature: z.object({
        celsius: z.number().nullable(),
        available: z.boolean(),
        valid: z.boolean(),
        age_ms: z.number().nullable(),
      }),
      fan: z.object({
        on: z.boolean(),
        speed_percent: z.number(),
        rpm: z.number(),
        mode: z.string(),
      }),
      rtc: z.object({
        available: z.boolean(),
        time: z.string().nullable(),
        lost_power: z.boolean(),
      }),
      wifi: z.object({
        rssi: z.number().nullable(),
        ip: z.string().nullable(),
        reconnects: z.number(),
      }),
      /**
       * Nulo até o primeiro POST que já mencione o alimentador. Diferente de
       * `feeder.connected === false`: nulo é "nunca ouvi falar desse módulo",
       * falso é "conheço o módulo e ele não está respondendo agora" — a
       * distinção é o que deixa a tela do alimentador escolher entre "isto
       * não existe ainda" e "isto existe e está desconectado".
       */
      feeder: feederStateSchema.nullable(),
      uptime_ms: z.number().nullable(),
      updated_at: isoInstantSchema,
      /**
       * Versão da configuração que o **dispositivo** diz estar usando. Igual ao
       * `config_version` da raiz quando a última mudança feita no app já chegou
       * ao aquário — é o que deixa a interface dizer "aplicado", e não só
       * "salvo no servidor".
       */
      config_version: z.int(),
      /**
       * Diagnóstico do **controlador**, não do aquário — acrescentado na
       * rodada de confiabilidade de 2026-08-21 (UPGRADE/06). Responde "o
       * ESP32 reiniciou esta noite, e por quê?" e "a memória está caindo?",
       * perguntas que o resto de `state` não tinha como responder.
       */
      controller: z
        .object({
          reset_reason: z.string().nullable(),
          free_heap: z.number().nullable(),
          min_free_heap: z.number().nullable(),
          max_alloc_heap: z.number().nullable(),
          post_latency_ms: z.number().nullable(),
          api_failures: z.number(),
          nvs_failures: z.number(),
          ota_last_result: z.string(),
          events_dropped: z.number(),
        })
        .nullable(),
    })
    .nullable(),
  config: deviceConfigSchema.nullable(),
  config_version: z.int(),
  /** Último autodiagnóstico executado. Nulo se nunca rodou. */
  diagnostic: diagnosticResultSchema.nullable(),
});
export type OverviewResponse = z.infer<typeof overviewResponseSchema>;

export const eventItemSchema = z.object({
  id: z.string(),
  received_at: isoInstantSchema,
  device_time: isoInstantSchema.nullable(),
  source: eventSourceSchema,
  sev: severitySchema,
  comp: componentSchema,
  code: eventCodeSchema,
  msg: z.string(),
  ctx: z.record(z.string(), z.unknown()).nullable(),
  repeat_count: z.int(),
});
export type EventItem = z.infer<typeof eventItemSchema>;

export const eventsResponseSchema = z.object({
  items: z.array(eventItemSchema),
  /** `<iso>|<uuid>` da última linha, ou nulo quando acabou. */
  next_cursor: z.string().nullable(),
});
export type EventsResponse = z.infer<typeof eventsResponseSchema>;

export const commandItemSchema = z.object({
  id: z.int(),
  target: commandTargetSchema,
  action: z.string(),
  payload: z.record(z.string(), z.unknown()),
  status: commandStatusSchema,
  requested_by: z.string().nullable(),
  created_at: isoInstantSchema,
  sent_at: isoInstantSchema.nullable(),
  settled_at: isoInstantSchema.nullable(),
  error_code: z.string().nullable(),
});
export type CommandItem = z.infer<typeof commandItemSchema>;

export const commandsResponseSchema = z.object({
  items: z.array(commandItemSchema),
});

export const reportPointSchema = z.object({
  hour: isoInstantSchema,
  temp_min: z.number().nullable(),
  temp_avg: z.number().nullable(),
  temp_max: z.number().nullable(),
  light_minutes: z.int(),
  fan_minutes: z.int(),
  fan_rpm_avg: z.int().nullable(),
  samples: z.int(),
});
export type ReportPoint = z.infer<typeof reportPointSchema>;

export const reportResponseSchema = z.object({
  items: z.array(reportPointSchema),
});

export const settingsResponseSchema = z.object({
  config: deviceConfigSchema.nullable(),
  config_version: z.int(),
});

/** Corpo aceito por `PUT /api/settings`. */
export const settingsUpdateSchema = z.object({
  light_on_time: timeOfDaySchema,
  light_off_time: timeOfDaySchema,
  fan_trigger_c: z.number(),
  fan_off_c: z.number(),
  telemetry_interval_ms: z.int(),
  heartbeat_interval_ms: z.int(),
});

/** Resposta de `POST /api/commands` — 202, o comando ainda não foi executado. */
export const commandAcceptedSchema = z.object({
  id: z.int(),
  status: z.literal("queued"),
});

// ── Notificações push ───────────────────────────────────────────────────────

/**
 * Inscrição de Web Push, no formato que `PushSubscription.toJSON()` produz.
 *
 * Validado no servidor porque vem do navegador: um `endpoint` malformado só
 * seria descoberto na hora de enviar o alerta — ou seja, exatamente quando
 * não dá para consertar.
 */
export const pushSubscriptionSchema = z.object({
  endpoint: z.url().max(512),
  keys: z.object({
    p256dh: z.string().min(1).max(256),
    auth: z.string().min(1).max(256),
  }),
});
export type PushSubscriptionInput = z.infer<typeof pushSubscriptionSchema>;

/** Corpo que o service worker recebe no evento `push`. */
export const pushPayloadSchema = z.object({
  title: z.string().max(80),
  body: z.string().max(240),
  tag: z.string().max(40),
  requireInteraction: z.boolean().optional(),
});
export type PushPayload = z.infer<typeof pushPayloadSchema>;
