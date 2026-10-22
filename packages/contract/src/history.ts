import { z } from "zod";
import { isoInstantSchema } from "./primitives.js";

/**
 * Histórico da ventoinha e da temperatura — a aba Ventoinha do app.
 *
 * Vem da telemetria bruta (uma linha a cada mudança ou a cada minuto), não do
 * rollup horário: o rollup só tem uma média por hora, e "qual era a
 * temperatura às 14:35?" precisa de mais resolução que isso.
 */

/** Os três períodos que o app oferece. */
export const HISTORY_RANGES = ["24h", "7d", "30d"] as const;
export const historyRangeSchema = z.enum(HISTORY_RANGES);
export type HistoryRange = z.infer<typeof historyRangeSchema>;

/**
 * Um ponto da série: `[início do intervalo em segundos Unix, média, mínima,
 * máxima]`, em °C com duas casas.
 *
 * Tupla, não objeto: são até 1.440 pontos por resposta, e repetir quatro nomes
 * de campo em cada um mais que dobrava o corpo.
 */
export const temperaturePointSchema = z.tuple([
  z.number(),
  z.number(),
  z.number(),
  z.number(),
]);
export type TemperaturePoint = z.infer<typeof temperaturePointSchema>;

/**
 * Uma rodada da ventoinha: do instante em que ligou ao instante em que
 * desligou. É o que responde "ela está dando conta?" — quanto tempo girou, de
 * que temperatura partiu e onde chegou.
 */
export const fanRunSchema = z.object({
  id: z.int(),
  started_at: isoInstantSchema,
  /** Nulo enquanto a ventoinha segue ligada. */
  ended_at: isoInstantSchema.nullable(),
  /** Segundos girando. Em rodada aberta, até agora. */
  duration_s: z.int(),
  /** Como a rodada começou: pela temperatura, por alguém, ou sem sensor. */
  mode: z.enum(["auto", "manual", "failsafe"]),
  /** Alguém mexeu (app, botão ou potenciômetro) durante a rodada. */
  manual: z.boolean(),
  /** Maior potência da rodada, em %. */
  speed_max: z.int(),
  temp_start: z.number().nullable(),
  temp_end: z.number().nullable(),
  /** Maior e menor temperatura vistas com a ventoinha ligada. */
  temp_peak: z.number().nullable(),
  temp_low: z.number().nullable(),
  /**
   * O aquário ficou sem contato durante a rodada (ou antes dela começar a ser
   * vista): início ou fim são o melhor palpite, não o instante exato.
   */
  approx: z.boolean(),
});
export type FanRun = z.infer<typeof fanRunSchema>;

export const fanHistorySummarySchema = z.object({
  /** Rodadas que tocam o período. */
  runs: z.int(),
  /** Segundos de ventoinha ligada dentro do período. */
  on_seconds: z.int(),
  /** Duração média das rodadas já encerradas; nulo sem nenhuma. */
  avg_duration_s: z.int().nullable(),
  /**
   * Queda média de temperatura por rodada automática encerrada
   * (`temp_start - temp_end`); nulo sem nenhuma com as duas leituras.
   */
  avg_drop_c: z.number().nullable(),
});

export const fanHistoryResponseSchema = z.object({
  range: historyRangeSchema,
  from: isoInstantSchema,
  to: isoInstantSchema,
  /** Largura de cada ponto de `points`, em segundos. */
  bucket_s: z.int(),
  /** Silêncio a partir do qual a linha do gráfico é interrompida, em segundos. */
  gap_s: z.int(),
  points: z.array(temperaturePointSchema),
  /** Extremos exatos do período, com o instante em que ocorreram. */
  temperature: z
    .object({
      min: z.number(),
      min_at: isoInstantSchema,
      avg: z.number(),
      max: z.number(),
      max_at: isoInstantSchema,
    })
    .nullable(),
  /** Mais recentes primeiro. Truncado em `runs_total` quando passa do teto. */
  runs: z.array(fanRunSchema),
  runs_total: z.int(),
  summary: fanHistorySummarySchema,
});
export type FanHistoryResponse = z.infer<typeof fanHistoryResponseSchema>;
