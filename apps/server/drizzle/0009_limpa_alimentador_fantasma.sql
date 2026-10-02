-- Só dados, nenhuma mudança de schema: apaga a "agenda" de um alimentador que
-- nunca existiu.
--
-- O firmware até a 2.0.0 mandava o bloco `feeder` inteiro em todo POST,
-- zerado quando o módulo nunca tinha respondido: o servidor gravou agenda
-- 00h/00h, 1 grão (o 0 grampeado ao mínimo), automático desligado e "última
-- alimentação falhou". Com isso o `/overview` deixou de devolver `feeder: null`,
-- e o app passou a mostrar um módulo "conhecido e desconectado" no lugar de
-- "nenhum módulo detectado".
--
-- A partir da v1.1.1 o contrato ignora esses campos quando `connected` é
-- falso, e o último valor conhecido fica preservado — inclusive este, que por
-- isso precisa ser limpo uma vez.
--
-- Escopo de propósito estreito, combinado com quem opera a produção: só o
-- dispositivo de produção, e só com a assinatura exata dos zeros (uma agenda
-- real nunca tem as duas refeições à meia-noite com 1 grão e nenhuma
-- alimentação). Idempotente: depois de rodar, a assinatura deixa de bater.
-- `feeder_connected` fica de fora — é NOT NULL e já vale false.
UPDATE "device_state"
SET
  "feeder_auto_enabled" = NULL,
  "feeder_hour1" = NULL,
  "feeder_hour2" = NULL,
  "feeder_grains_per_feeding" = NULL,
  "feeder_last_feed_requested" = NULL,
  "feeder_last_feed_confirmed" = NULL,
  "feeder_last_feed_ok" = NULL
WHERE "device_id" = 'aquarium-01'
  AND "feeder_connected" = false
  AND "feeder_auto_enabled" = false
  AND "feeder_hour1" = 0
  AND "feeder_hour2" = 0
  AND "feeder_grains_per_feeding" = 1
  AND "feeder_last_feed_at" IS NULL
  AND "feeder_last_feed_requested" = 0
  AND "feeder_last_feed_confirmed" = 0
  AND "feeder_last_feed_ok" = false;
