-- Manutenção das tabelas particionadas: criação adiantada e purga por tamanho.
--
-- Escrita como funções SQL em vez de código TypeScript de propósito: são
-- operações de catálogo (CREATE TABLE, DROP TABLE) que precisam ser atômicas e
-- idempotentes. Fazê-las daqui evita uma dança de transações do lado do Node.

-- ─────────────────────────────────────────────────────────────────────────────
-- Cria a partição mensal de uma tabela, se ainda não existir.
-- Idempotente: rodar dez vezes tem o mesmo efeito que rodar uma.
-- ─────────────────────────────────────────────────────────────────────────────
CREATE OR REPLACE FUNCTION bettacare_ensure_partition(
  p_table text,
  p_month date
) RETURNS text AS $$
DECLARE
  v_start date := date_trunc('month', p_month)::date;
  v_end   date := (date_trunc('month', p_month) + interval '1 month')::date;
  v_name  text := format('%s_%s', p_table, to_char(v_start, 'YYYY_MM'));
BEGIN
  IF to_regclass(format('public.%I', v_name)) IS NULL THEN
    EXECUTE format(
      'CREATE TABLE %I PARTITION OF %I FOR VALUES FROM (%L) TO (%L)',
      v_name, p_table, v_start, v_end
    );
  END IF;
  RETURN v_name;
END;
$$ LANGUAGE plpgsql;
--> statement-breakpoint

-- ─────────────────────────────────────────────────────────────────────────────
-- Garante a partição do mês corrente e a do mês seguinte.
--
-- A do mês seguinte é criada adiantada porque um INSERT sem partição
-- correspondente falha — e a virada do mês acontece às 00:00, quando ninguém
-- está olhando.
-- ─────────────────────────────────────────────────────────────────────────────
CREATE OR REPLACE FUNCTION bettacare_ensure_current_partitions()
RETURNS void AS $$
DECLARE
  v_table text;
BEGIN
  FOREACH v_table IN ARRAY ARRAY['telemetry', 'events'] LOOP
    PERFORM bettacare_ensure_partition(v_table, CURRENT_DATE);
    PERFORM bettacare_ensure_partition(v_table, (CURRENT_DATE + interval '1 month')::date);
  END LOOP;
END;
$$ LANGUAGE plpgsql;
--> statement-breakpoint

-- ─────────────────────────────────────────────────────────────────────────────
-- Purga por tamanho.
--
-- Enquanto o banco exceder o teto, derruba a partição mais antiga entre
-- `telemetry` e `events` — a maior primeiro, para convergir em menos passos.
--
-- Três invariantes que não podem cair:
--   1. A partição do mês corrente nunca é derrubada. Se sobrar só ela e o teto
--      continuar estourado, a função para e devolve o fato. Um teto inatingível
--      é problema para um humano resolver, não para o job insistir.
--   2. DROP TABLE devolve espaço ao sistema de arquivos na hora. DELETE não —
--      é a razão de tudo isto existir.
--   3. `telemetry_hourly`, `settings`, `commands`, `devices` e
--      `component_status` são isentos: somam poucos MB e são o que mantém os
--      relatórios funcionando sobre períodos já purgados.
-- ─────────────────────────────────────────────────────────────────────────────
CREATE OR REPLACE FUNCTION bettacare_enforce_size_limit(p_limit_bytes bigint)
RETURNS TABLE (dropped_partition text, freed_bytes bigint) AS $$
DECLARE
  v_size      bigint;
  v_candidate record;
  v_guard     int := 0;
BEGIN
  LOOP
    v_guard := v_guard + 1;
    EXIT WHEN v_guard > 240;  -- 20 anos de partições mensais; laço infinito jamais

    SELECT pg_database_size(current_database()) INTO v_size;
    EXIT WHEN v_size <= p_limit_bytes;

    -- A partição mais antiga, maior primeiro.
    --
    -- O filtro é `estritamente anterior ao mês corrente`, e não apenas
    -- `diferente do mês corrente`: a partição do mês *seguinte* é criada
    -- adiantada e está sempre vazia. Derrubá-la não liberaria nada e deixaria
    -- a virada do mês sem destino para o INSERT.
    SELECT c.relname AS name, pg_total_relation_size(c.oid) AS bytes
      INTO v_candidate
      FROM pg_class c
      JOIN pg_inherits i ON i.inhrelid = c.oid
      JOIN pg_class p ON p.oid = i.inhparent
     WHERE p.relname IN ('telemetry', 'events')
       AND substring(c.relname from '\d{4}_\d{2}$')
           < to_char(CURRENT_DATE, 'YYYY_MM')
     ORDER BY substring(c.relname from '\d{4}_\d{2}$') ASC,
              pg_total_relation_size(c.oid) DESC
     LIMIT 1;

    EXIT WHEN NOT FOUND;

    EXECUTE format('DROP TABLE %I', v_candidate.name);

    dropped_partition := v_candidate.name;
    freed_bytes := v_candidate.bytes;
    RETURN NEXT;
  END LOOP;
END;
$$ LANGUAGE plpgsql;
--> statement-breakpoint

-- Cria as partições iniciais para que o primeiro POST não esbarre em tabela
-- ausente. Idempotente, como todo o resto.
SELECT bettacare_ensure_current_partitions();
