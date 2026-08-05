import { z } from "zod";
import { isoInstantSchema } from "./primitives.js";

/**
 * Comandos do servidor para o dispositivo.
 *
 * **Toda ação é estado desejado, nunca alternância.** O `light.toggle` do
 * sistema antigo foi eliminado de propósito: o contrato exige tolerar reenvio,
 * e um toggle reentregue inverteria o estado duas vezes. `light.set{on:true}`
 * reenviado dez vezes tem o mesmo efeito que uma.
 */
export const commandActionSchema = z.discriminatedUnion("action", [
  z.object({
    action: z.literal("light.set"),
    on: z.boolean(),
  }),
  z.object({
    action: z.literal("fan.set_speed"),
    percent: z.int().min(0).max(100),
  }),
  z.object({
    action: z.literal("fan.set_mode"),
    mode: z.enum(["auto", "manual"]),
  }),
  z.object({
    /** Manda o dispositivo reler a configuração devolvida no POST. */
    action: z.literal("config.apply"),
  }),
  z.object({
    action: z.literal("device.reboot"),
  }),
]);
export type CommandAction = z.infer<typeof commandActionSchema>;

export const COMMAND_ACTION_NAMES = [
  "light.set",
  "fan.set_speed",
  "fan.set_mode",
  "config.apply",
  "device.reboot",
] as const;

/**
 * O alvo do comando. Dois comandos com o mesmo alvo se anulam: enfileirar um
 * novo marca o anterior como `superseded`, para o dispositivo não executar uma
 * sequência de decisões já obsoletas depois de voltar de um período offline.
 */
export const commandTargetSchema = z.enum([
  "light",
  "fan",
  "config",
  "device",
]);
export type CommandTarget = z.infer<typeof commandTargetSchema>;

export function targetOf(action: CommandAction): CommandTarget {
  switch (action.action) {
    case "light.set":
      return "light";
    case "fan.set_speed":
    case "fan.set_mode":
      return "fan";
    case "config.apply":
      return "config";
    case "device.reboot":
      return "device";
  }
}

/** Comando como viaja na resposta do POST — o menor formato possível. */
export const pendingCommandSchema = z.intersection(
  z.object({ id: z.int().positive() }),
  commandActionSchema,
);
export type PendingCommand = z.infer<typeof pendingCommandSchema>;

/**
 * Confirmação vinda do dispositivo no POST seguinte.
 *
 * O contrato original previa `ack: [91]` — uma lista de números, que só sabe
 * dizer "recebi" e nunca "não consegui". Com o objeto, a falha volta com nome,
 * e é dela que sai a mensagem de erro que o usuário lê na interface.
 */
export const commandAckSchema = z.object({
  id: z.int().positive(),
  ok: z.boolean(),
  /** Presente quando `ok` é falso. Ex.: `light.gpio_fault`. */
  code: z.string().max(48).optional(),
});
export type CommandAck = z.infer<typeof commandAckSchema>;

/**
 * Ciclo de vida completo, como a interface o vê.
 *
 * ```
 * queued ──► sent ──► acked
 *    │        │
 *    │        ├────► rejected
 *    │        └────► expired
 *    └─────────────► superseded
 * ```
 */
export const commandStatusSchema = z.enum([
  "queued",
  "sent",
  "acked",
  "rejected",
  "expired",
  "superseded",
]);
export type CommandStatus = z.infer<typeof commandStatusSchema>;

export const COMMAND_STATUS_LABELS: Record<CommandStatus, string> = {
  queued: "Na fila",
  sent: "Enviado",
  acked: "Confirmado",
  rejected: "Recusado",
  expired: "Expirou",
  superseded: "Substituído",
};

/** Um comando já persistido, com a trilha inteira. */
export const storedCommandSchema = z.object({
  id: z.int().positive(),
  device_id: z.string(),
  target: commandTargetSchema,
  action: z.string(),
  payload: z.record(z.string(), z.unknown()),
  status: commandStatusSchema,
  /** E-mail vindo do Cloudflare Access — quem pediu. */
  requested_by: z.string().nullable(),
  created_at: isoInstantSchema,
  sent_at: isoInstantSchema.nullable(),
  settled_at: isoInstantSchema.nullable(),
  /** Motivo, quando não deu certo. */
  error_code: z.string().nullable(),
});
export type StoredCommand = z.infer<typeof storedCommandSchema>;

/**
 * Prazo para o dispositivo confirmar um comando já entregue. Passou disso,
 * o watchdog marca `expired`. Generoso de propósito: são 30 ciclos de POST.
 */
export const DEFAULT_COMMAND_TTL_S = 90;
