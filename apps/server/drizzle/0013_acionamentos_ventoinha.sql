CREATE TABLE "fan_runs" (
	"id" integer PRIMARY KEY GENERATED ALWAYS AS IDENTITY (sequence name "fan_runs_id_seq" INCREMENT BY 1 MINVALUE 1 MAXVALUE 2147483647 START WITH 1 CACHE 1),
	"device_id" text NOT NULL,
	"started_at" timestamp with time zone NOT NULL,
	"ended_at" timestamp with time zone,
	"last_on_at" timestamp with time zone NOT NULL,
	"mode" "fan_mode" DEFAULT 'auto' NOT NULL,
	"manual" boolean DEFAULT false NOT NULL,
	"speed_max" smallint DEFAULT 0 NOT NULL,
	"temp_start" real,
	"temp_end" real,
	"temp_peak" real,
	"temp_low" real,
	"approx" boolean DEFAULT false NOT NULL
);
--> statement-breakpoint
ALTER TABLE "fan_runs" ADD CONSTRAINT "fan_runs_device_id_devices_device_id_fk" FOREIGN KEY ("device_id") REFERENCES "public"."devices"("device_id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
CREATE INDEX "fan_runs_device_started_idx" ON "fan_runs" USING btree ("device_id","started_at");--> statement-breakpoint
CREATE UNIQUE INDEX "fan_runs_one_open_idx" ON "fan_runs" USING btree ("device_id") WHERE "fan_runs"."ended_at" is null;--> statement-breakpoint
-- Reconstrói, uma única vez, as rodadas que já estão na telemetria bruta, para
-- o histórico não nascer vazio no dia do deploy. É só leitura de `telemetry`
-- e escrita na tabela nova: nada existente é alterado nem apagado.
--
-- Uma rodada é uma sequência de linhas com a ventoinha ligada. O fim é a
-- linha seguinte (a que registra o desligamento): o ingest grava uma linha a
-- cada mudança de estado, então ela existe. Silêncio de mais de 5 min dentro
-- ou nas bordas da rodada marca `approx`, e então o fim é a última linha
-- ligada — um limite inferior, que é o que se sabe — sem temperatura de saída.
INSERT INTO "fan_runs"
  ("device_id", "started_at", "ended_at", "last_on_at", "mode", "manual",
   "speed_max", "temp_start", "temp_end", "temp_peak", "temp_low", "approx")
WITH linhas AS (
  SELECT t.device_id, t.received_at, t.fan_on, t.fan_mode, t.fan_speed_percent,
         CASE WHEN t.temp_valid THEN t.temp_celsius END AS temp,
         lag(t.fan_on)       OVER w AS ligada_antes,
         lag(t.received_at)  OVER w AS anterior_em,
         lead(t.received_at) OVER w AS proxima_em,
         lead(t.fan_on)      OVER w AS proxima_ligada,
         lead(CASE WHEN t.temp_valid THEN t.temp_celsius END) OVER w AS proxima_temp
    FROM telemetry t
   WHERE EXISTS (SELECT 1 FROM devices d WHERE d.device_id = t.device_id)
  WINDOW w AS (PARTITION BY t.device_id ORDER BY t.received_at)
),
numeradas AS (
  SELECT *,
         sum(CASE WHEN fan_on AND NOT coalesce(ligada_antes, false) THEN 1 ELSE 0 END)
           OVER (PARTITION BY device_id ORDER BY received_at) AS rodada
    FROM linhas
),
rodadas AS (
  SELECT device_id, rodada,
         min(received_at) AS inicio,
         max(received_at) AS ultima_ligada,
         (array_agg(fan_mode ORDER BY received_at))[1] AS modo,
         bool_or(fan_mode IN ('manual', 'manual_off')) AS manual,
         max(fan_speed_percent) AS potencia,
         (array_agg(temp ORDER BY received_at))[1] AS temp_inicio,
         max(temp) AS pico,
         min(temp) AS minima,
         (array_agg(proxima_em   ORDER BY received_at DESC))[1] AS fim_visto_em,
         (array_agg(proxima_temp ORDER BY received_at DESC))[1] AS temp_fim,
         coalesce((array_agg(received_at - anterior_em ORDER BY received_at))[1] > interval '5 minutes', true) AS inicio_apos_lacuna,
         coalesce(bool_or(proxima_ligada AND proxima_em - received_at > interval '5 minutes'), false) AS lacuna_no_meio
    FROM numeradas
   WHERE fan_on
   GROUP BY device_id, rodada
)
SELECT r.device_id,
       r.inicio,
       CASE WHEN r.fim_visto_em IS NULL THEN NULL
            WHEN r.fim_visto_em - r.ultima_ligada > interval '5 minutes' THEN r.ultima_ligada
            ELSE r.fim_visto_em END,
       r.ultima_ligada,
       r.modo,
       r.manual,
       r.potencia,
       r.temp_inicio,
       CASE WHEN r.fim_visto_em IS NULL OR r.fim_visto_em - r.ultima_ligada > interval '5 minutes' THEN NULL
            ELSE r.temp_fim END,
       r.pico,
       r.minima,
       (r.inicio_apos_lacuna OR r.lacuna_no_meio
         OR coalesce(r.fim_visto_em - r.ultima_ligada > interval '5 minutes', false))
  FROM rodadas r
 ORDER BY r.device_id, r.inicio;
