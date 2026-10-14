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
  z.object({
    /**
     * Pede um autodiagnóstico: o firmware sonda ativamente cada periférico e
     * devolve o relatório no POST seguinte (ver `diagnosticReportSchema`).
     *
     * Não altera estado nenhum do aquário — só lê barramentos e faz uma
     * escrita de teste numa chave própria da NVS. Foi desenhado assim de
     * propósito: um diagnóstico que mexe nos atuadores é um diagnóstico que
     * ninguém roda com o aquário povoado.
     */
    action: z.literal("device.diagnose"),
  }),
  z.object({
    /**
     * Alimenta agora, fora da agenda. O módulo do alimentador (quando
     * conectado) confirma via UART e o resultado real — grãos pedidos ×
     * confirmados — chega num POST seguinte, no bloco `feeder` da telemetria,
     * não neste `ack`. O `ack` aqui só diz "o ESP32 repassou o pedido pro
     * módulo", o mesmo desenho de `device.diagnose`.
     */
    action: z.literal("feeder.feed_now"),
    /** Grãos a dispensar; omitido usa o valor configurado no módulo. */
    grains: z.int().min(1).max(20).optional(),
    /**
     * Alimenta mesmo com o limite de refeições em 24 h atingido
     * (`FEEDER_MEALS_PER_24H`). Só existe aqui, no app: o botão do módulo
     * nunca passa do limite. Sem isto, o módulo recusa e o servidor
     * registra `feeder.limit_reached`.
     */
    force: z.boolean().optional(),
  }),
  z.object({
    /**
     * Substitui a agenda inteira do alimentador — nunca um campo isolado.
     * `feeder.feed_now` e `feeder.set_config` compartilham o alvo `"feeder"`
     * de propósito (mesmo padrão de `device.reboot`/`device.diagnose`): as
     * duas ações são raras o bastante para aceitar o mesmo trade-off de
     * `superseded` que já existe ali.
     *
     * A NVS do módulo continua sendo a fonte de verdade — isto é só o caminho
     * de escrita quando a edição vem do app. O módulo confirma aplicando e
     * devolvendo o novo estado no `feeder` da telemetria seguinte.
     */
    action: z.literal("feeder.set_config"),
    hour1: z.int().min(0).max(23),
    hour2: z.int().min(0).max(23),
    grains_per_feeding: z.int().min(1).max(20),
    auto_enabled: z.boolean(),
  }),
]);
export type CommandAction = z.infer<typeof commandActionSchema>;

export const COMMAND_ACTION_NAMES = [
  "light.set",
  "fan.set_speed",
  "fan.set_mode",
  "config.apply",
  "device.reboot",
  "device.diagnose",
  "feeder.feed_now",
  "feeder.set_config",
] as const;
export type CommandActionName = (typeof COMMAND_ACTION_NAMES)[number];

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
  "feeder",
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
    case "device.diagnose":
      return "device";
    case "feeder.feed_now":
    case "feeder.set_config":
      return "feeder";
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
 *    │        └────► expired     (prazo esgotado sem ack, ou reinício no meio)
 *    ├─────────────► expired     (nunca entregue dentro do prazo)
 *    └─────────────► superseded
 * ```
 *
 * `sent` não é "o dispositivo recebeu": é "foi numa resposta de POST". Com o
 * Wi-Fi ruim do aquário, a resposta se perde com frequência. Por isso o
 * comando continua sendo reentregue a cada POST até o `ack`, dentro do mesmo
 * prazo de vida de quando estava na fila (`DEFAULT_COMMAND_MAX_AGE_S`).
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
 * Prazo de vida de um comando, contado da criação: até ele, o comando espera
 * na fila e é (re)entregue a cada POST; passado dele, o watchdog o encerra
 * como `expired`.
 *
 * 15 min saíram do histórico de produção (2026-10-07 a 2026-10-14, Wi-Fi do
 * aquário): das 103 quedas, 87% duraram menos que isso (mediana de 2,3 min,
 * p90 de 13 min). Esticar para 30 min só cobria mais 3%; o resto são apagões
 * de horas, e executar "acender a luz" horas depois do pedido seria pior que
 * não executar. Era 90 s contados da primeira entrega: uma resposta perdida
 * logo antes de uma queda bastava para o comando morrer, já executado ou não.
 */
export const DEFAULT_COMMAND_MAX_AGE_S = 900;

/**
 * Ações que **não** podem ser repetidas só porque a confirmação se perdeu:
 * uma segunda alimentação é ração a mais na água, e um segundo reinício é
 * mais um apagão da luminária.
 *
 * O firmware desde a 2.1.1 reconhece a reentrega pelo `id` e confirma sem
 * executar de novo. O que ele não tem como lembrar é o que aconteceu antes de
 * um reinício — então, quando o `boot_id` muda com uma dessas ainda `sent`, o
 * servidor a encerra em vez de reentregar (ver `ingest/process.ts`).
 */
export const NON_REPEATABLE_ACTIONS: ReadonlySet<CommandActionName> = new Set<CommandActionName>([
  "feeder.feed_now",
  "device.reboot",
]);
