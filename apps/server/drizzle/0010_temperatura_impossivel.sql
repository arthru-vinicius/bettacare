-- Só dados, nenhuma mudança de schema: tira do histórico as leituras de
-- temperatura impossíveis e refaz as horas do rollup que elas contaminaram.
--
-- Em produção: dois -48,00 °C isolados (2026-10-01 23:00:31Z e 23:00:34Z),
-- entre leituras de 27,13 °C — um quadro de 1-Wire corrompido que passou no
-- CRC por acaso. A hora 23:00Z ficou com mínima -48 e a média 1,5 °C abaixo
-- do real, e o gráfico de 24 h e de 7 dias esticava o eixo até -48.
--
-- A faixa é a de TEMP_PLAUSIBLE_C (10–45 °C), em
-- packages/contract/src/telemetry.ts. Daqui em diante o firmware 2.0.2 e o
-- ingest descartam a leitura impossível na origem; isto acerta o que já
-- estava gravado, e **só o impossível**. O job novo agrega só a faixa
-- habitual (16–33 °C) e deixa o resto como aviso com duração e pico
-- (temp.out_of_usual_range) — mas esse aviso só existe a partir da v1.1.2.
-- Aplicar a regra habitual ao passado apagaria em silêncio o extremo de uma
-- hora quente ou fria de verdade, sem aviso nenhum no lugar. (Revisão do
-- homelab-c4, com dry-run em produção.)
--
-- Idempotente: depois de rodar, nenhuma das duas condições volta a bater.

-- 1. A leitura impossível sai do histórico bruto. `temp_valid` acompanha:
--    não era temperatura válida, mesmo tendo passado no CRC.
UPDATE "telemetry"
SET "temp_celsius" = NULL,
    "temp_valid" = false
WHERE "temp_celsius" < 10 OR "temp_celsius" > 45;
--> statement-breakpoint

-- 2. As horas do rollup contaminadas por leitura impossível são refeitas a
--    partir do bruto já limpo, só sem o impossível: o extremo real de uma
--    hora, se houver, fica. `samples` não muda — é a contagem de linhas
--    brutas da hora (cobertura), a mesma regra do job, e as linhas limpas
--    continuam lá. Hora cujo bruto já foi purgado não aparece na
--    subconsulta e fica como está.
UPDATE "telemetry_hourly" h
SET "temp_min" = s.temp_min,
    "temp_avg" = s.temp_avg,
    "temp_max" = s.temp_max
FROM (
  SELECT "device_id",
         date_trunc('hour', "received_at") AS hour,
         (min("temp_celsius") FILTER (WHERE "temp_celsius" BETWEEN 10 AND 45))::real AS temp_min,
         (avg("temp_celsius") FILTER (WHERE "temp_celsius" BETWEEN 10 AND 45))::real AS temp_avg,
         (max("temp_celsius") FILTER (WHERE "temp_celsius" BETWEEN 10 AND 45))::real AS temp_max
  FROM "telemetry"
  GROUP BY 1, 2
) s
WHERE h."device_id" = s."device_id"
  AND h."hour" = s.hour
  AND (h."temp_min" < 10 OR h."temp_max" > 45);
