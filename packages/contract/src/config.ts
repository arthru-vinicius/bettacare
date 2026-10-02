import { z } from "zod";
import { timeOfDaySchema } from "./primitives.js";

/**
 * Configuração do aquário.
 *
 * Com o banco no jogo, o **servidor passa a ser a fonte de verdade** e o NVS do
 * ESP32 vira cache offline. O dispositivo compara o `config_version` a cada POST
 * e aplica quando diverge — e continua operando sozinho, com a última
 * configuração conhecida, se o servidor sumir.
 */
export const deviceConfigSchema = z
  .object({
    light_on_time: timeOfDaySchema,
    light_off_time: timeOfDaySchema,
    /** Temperatura em que a ventoinha liga no modo automático. */
    fan_trigger_c: z.number().min(15).max(40),
    /** Temperatura em que a ventoinha inicia o desligamento. */
    fan_off_c: z.number().min(15).max(40),
    /** Intervalo do POST. Ajustável pelo servidor, sem regravar o firmware. */
    telemetry_interval_ms: z.int().min(1000).max(60000),
    /** Grava uma linha de histórico mesmo sem mudança, a cada tanto. */
    heartbeat_interval_ms: z.int().min(10000).max(600000),
  })
  .refine((c) => c.fan_off_c < c.fan_trigger_c, {
    message:
      "fan_off_c precisa ser menor que fan_trigger_c — sem essa folga a ventoinha oscilaria sem parar",
    path: ["fan_off_c"],
  })
  /**
   * Horários iguais não são "nunca acende": o firmware lê a janela como cruzando
   * a meia-noite (`on <= agora || agora < off`), que com os dois iguais é
   * verdade o dia inteiro — a luz ficaria acesa para sempre.
   */
  .refine((c) => c.light_on_time !== c.light_off_time, {
    message: "o horário de acender precisa ser diferente do de apagar",
    path: ["light_off_time"],
  });

export type DeviceConfig = z.infer<typeof deviceConfigSchema>;

export const DEFAULT_CONFIG: DeviceConfig = {
  light_on_time: "10:00",
  light_off_time: "17:00",
  fan_trigger_c: 29.0,
  fan_off_c: 27.5,
  telemetry_interval_ms: 3000,
  heartbeat_interval_ms: 60000,
};
