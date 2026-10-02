import {
  COMPONENT_LABELS,
  DEFAULT_CONFIG,
  deviceConfigSchema,
  HEALTH_STATUS_LABELS,
  TEMP_PLAUSIBLE_C,
  TEMP_USUAL_C,
  type Component,
  type DeviceConfig,
  type PendingCommand,
  type HealthStatus,
  type TelemetryCorrection,
  type TelemetryRequest,
  type TelemetryResponse,
} from "@bettacare/contract";
import { and, asc, desc, eq, gte, inArray, sql } from "drizzle-orm";

import type { AppConfig } from "../config.js";
import type { PushMessage } from "../push/notify.js";
import type { Db } from "../db/client.js";
import {
  commands,
  componentStatus,
  deviceState,
  devices,
  events,
  settings,
  telemetry,
} from "../db/schema.js";
import { evaluateHealth, shouldWriteHistory } from "../health/evaluate.js";

/**
 * Quantos comandos cabem numa resposta. O corpo precisa continuar pequeno e
 * previsível — o ESP32 tem memória escassa — e na prática nunca há mais que um
 * ou dois pendentes, porque comandos do mesmo alvo se anulam.
 */
const MAX_COMMANDS_PER_RESPONSE = 4;

export interface ProcessResult {
  response: TelemetryResponse;
  /** Verdadeiro quando o POST foi um reenvio e nada foi gravado. */
  duplicate: boolean;
  /** Se uma linha de histórico foi escrita neste ciclo. */
  wroteHistory: boolean;
  /**
   * Alertas a notificar, coletados durante a transação e enviados **depois**
   * que ela fecha.
   *
   * A ordem importa: mandar push de dentro da transação significaria segurar
   * uma conexão do pool durante uma chamada HTTP a um push service externo —
   * que pode demorar segundos ou pendurar. O caminho quente do ingest não pode
   * ficar refém disso.
   */
  alerts: PushMessage[];
  /**
   * Correções que abriram evento neste POST. As repetidas só somaram
   * `repeat_count` num evento já aberto, e o router as rebaixa a `debug` —
   * senão o mesmo campo errado vira um `warn` por segundo no log do homelab.
   */
  newCorrections: TelemetryCorrection[];
}

/**
 * Status que merecem interromper alguém.
 *
 * `degraded` fica de fora de propósito: RSSI fraco ou leitura velha são
 * coisas para olhar quando der, não para tocar o celular à noite. Notificação
 * que chega demais deixa de ser lida.
 */
const STATUS_QUE_ALERTAM = new Set<HealthStatus>(["fault", "missing"]);

/**
 * Fora de uma transição, `component_status` só é regravado a cada tanto.
 *
 * Antes eram onze upserts por POST, mudasse algo ou não — com POST a cada 1 s,
 * quase um milhão de escritas por dia para guardar "continua tudo igual". O
 * que precisa ser imediato (status, código, início de anomalia) continua
 * imediato; o valor exibido na aba Saúde (°C, rpm) e o "último ok" só
 * envelhecem até este intervalo.
 */
const HEALTH_REFRESH_MS = 5_000;

export async function processTelemetry(
  db: Db,
  cfg: AppConfig,
  received: TelemetryRequest,
  now: Date,
  /** Campos que o saneamento (UPGRADE/05, C1) precisou corrigir antes de aceitar o corpo. */
  corrections: readonly TelemetryCorrection[] = [],
): Promise<ProcessResult> {
  const alerts: PushMessage[] = [];
  /** O corpo como é processado: igual ao recebido, menos uma leitura impossível de temperatura. */
  let body = received;

  return db.transaction(async (tx) => {
    // ── 1. O dispositivo existe e acabou de falar ────────────────────────
    const [before] = await tx
      .select({ online: devices.online })
      .from(devices)
      .where(eq(devices.deviceId, body.device_id))
      .limit(1);

    await tx
      .insert(devices)
      .values({
        deviceId: body.device_id,
        lastSeenAt: now,
        lastIp: body.wifi.ip,
        fwVersion: body.fw_version ?? null,
        online: true,
      })
      .onConflictDoUpdate({
        target: devices.deviceId,
        set: {
          lastSeenAt: now,
          lastIp: body.wifi.ip,
          fwVersion: body.fw_version ?? null,
          online: true,
        },
      });

    // O watchdog marca offline; quem percebe a volta é o próprio POST.
    if (before !== undefined && !before.online) {
      await insertServerEvent(tx, body.device_id, now, {
        sev: "info",
        comp: "system",
        code: "device.online",
        msg: "Dispositivo voltou a se comunicar",
      });
    }

    // Um campo saiu de faixa e foi corrigido no lugar de derrubar o corpo
    // inteiro (UPGRADE/05, C1). O diagnóstico honesto: algo está errado nesse
    // campo específico, não que o dispositivo sumiu.
    const newCorrections: TelemetryCorrection[] = [];
    for (const c of corrections) {
      if (c.action === "dropped") {
        // Cada item descartado é uma ocorrência distinta do firmware — um
        // evento que teria sido gravado —, então um para um, sem agrupar.
        await insertServerEvent(tx, body.device_id, now, {
          sev: "warn",
          comp: "api",
          code: "ingest.event_dropped",
          msg: `Item "${c.path}" descartado por campo inválido; o resto do envio foi aceito`,
          ctx: { field: c.path, reason: c.message, received: c.received },
        });
        newCorrections.push(c);
        continue;
      }
      if (await countRepeatedEvent(tx, body.device_id, "ingest.field_rejected", now, c.path)) continue;
      await insertServerEvent(tx, body.device_id, now, {
        sev: "warn",
        comp: "api",
        code: "ingest.field_rejected",
        msg: `Campo "${c.path}" fora de faixa, corrigido para aceitar o resto do envio`,
        ctx: { field: c.path, reason: c.message, received: c.received },
      });
      newCorrections.push(c);
    }

    const [previous] = await tx
      .select()
      .from(deviceState)
      .where(eq(deviceState.deviceId, body.device_id))
      .limit(1);

    // ── 2. Confirmações do POST anterior ─────────────────────────────────
    // Processadas mesmo num reenvio: marcar um comando como confirmado é
    // idempotente, e perder um ack custaria uma reentrega desnecessária.
    const lightDesiredFromAck = await applyAcks(tx, body, now);

    // ── 3. Idempotência ──────────────────────────────────────────────────
    // A guarda é o `seq`, não uma constraint única: o particionamento de
    // `telemetry` impede um UNIQUE (device_id, boot_id, seq) de verdade.
    // Um `boot_id` novo zera a contagem — o dispositivo reiniciou.
    const duplicate =
      previous !== undefined &&
      body.boot_id === previous.lastBootId &&
      body.seq <= previous.lastSeq;

    const { config, configVersion } = await loadConfig(tx, body.device_id, now);

    let wroteHistory = false;

    if (!duplicate) {
      body = await discardImplausibleTemperature(tx, body, previous, now);

      /**
       * O "desejado" de um comando só vale enquanto a última mudança da luz
       * foi esse comando (UPGRADE/07). Quando o botão físico ou a automação
       * mudam a luz, o pedido antigo deixa de ser referência — mantê-lo fazia
       * qualquer uso legítimo do botão parecer falha da luminária.
       */
      const lightDesired =
        lightDesiredFromAck ??
        (body.light.source === "command" ? (previous?.lightDesired ?? null) : null);

      await recordHealth(tx, body, now, lightDesired, alerts);
      await recordDeviceEvents(tx, body, now);
      await trackUsualRange(
        tx,
        body.device_id,
        body.temperature.celsius,
        previous?.tempCelsius ?? null,
        now,
        cfg.TZ_DISPLAY,
      );

      wroteHistory = shouldWriteHistory(
        body,
        previous
          ? {
              lightOn: previous.lightOn,
              fanOn: previous.fanOn,
              fanSpeedPercent: previous.fanSpeedPercent,
              fanMode: previous.fanMode,
              tempCelsius: previous.tempCelsius,
              tempAvailable: previous.tempAvailable,
              lastHistoryAt: previous.lastHistoryAt,
            }
          : null,
        now,
        config.heartbeat_interval_ms,
      );

      if (wroteHistory) {
        await tx.insert(telemetry).values({
          receivedAt: now,
          deviceId: body.device_id,
          bootId: body.boot_id,
          seq: body.seq,
          deviceTime: body.device_time ? new Date(body.device_time) : null,
          lightOn: body.light.on,
          tempCelsius: body.temperature.celsius,
          tempValid: body.temperature.valid,
          fanOn: body.fan.on,
          fanSpeedPercent: body.fan.speed_percent,
          fanRpm: body.fan.rpm,
          fanMode: body.fan.mode,
          wifiRssi: body.wifi.rssi,
          // Histórico do controlador (UPGRADE/04, S9) — ver o comentário em
          // `db/schema.ts` sobre por que `resetReason` fica fora daqui.
          tempAgeMs: body.temperature.age_ms,
          rtcAvailable: body.rtc.available,
          rtcLostPower: body.rtc.lost_power,
          wifiReconnects: body.wifi.reconnects,
          freeHeap: body.diag?.free_heap ?? null,
          maxAllocHeap: body.diag?.max_alloc_heap ?? null,
          postLatencyMs: body.diag?.post_latency_ms ?? null,
          potRawAdc: body.diag?.pot_raw_adc ?? null,
        });
      }

      const diagnostic = await recordDiagnostic(tx, body, now);

      /**
       * Último conhecido, preservado quando o módulo não reportou nada
       * fresco neste ciclo — desconectar não pode apagar a agenda que o app
       * mostra (ele não fica ligado o tempo todo por desenho, ver
       * `docs/pinagem-alimentador-modulo.md`). `feederConnected` é a
       * exceção: reflete sempre o que **este** POST diz, nunca o anterior —
       * é o único campo que precisa ser "agora", não "última vez".
       */
      const f = body.feeder;
      const feederColumns = {
        feederConnected: f?.connected ?? false,
        feederAutoEnabled: f?.auto_enabled ?? previous?.feederAutoEnabled ?? null,
        feederHour1: f?.hour1 ?? previous?.feederHour1 ?? null,
        feederHour2: f?.hour2 ?? previous?.feederHour2 ?? null,
        feederGrainsPerFeeding:
          f?.grains_per_feeding ?? previous?.feederGrainsPerFeeding ?? null,
        feederLastFeedAt:
          f?.last_feed_age_s != null
            ? new Date(now.getTime() - f.last_feed_age_s * 1000)
            : (previous?.feederLastFeedAt ?? null),
        feederLastFeedRequested:
          f?.last_feed_requested ?? previous?.feederLastFeedRequested ?? null,
        feederLastFeedConfirmed:
          f?.last_feed_confirmed ?? previous?.feederLastFeedConfirmed ?? null,
        feederLastFeedOk: f?.last_feed_ok ?? previous?.feederLastFeedOk ?? null,
      };

      await upsertState(tx, body, now, {
        lightDesired,
        lastHistoryAt: wroteHistory ? now : (previous?.lastHistoryAt ?? null),
        diagnostic,
        feederColumns,
      });
    }

    // ── 4. Comandos para a resposta ──────────────────────────────────────
    const pending = await takePendingCommands(tx, body.device_id, now);

    const response: TelemetryResponse = {
      ok: true,
      server_time: now.toISOString(),
      config_version: configVersion,
      commands: pending,
      // O bloco só viaja quando o dispositivo está atrasado. No caso normal a
      // resposta fica em torno de 100 bytes.
      ...(body.config_version === configVersion ? {} : { config }),
    };

    return { response, duplicate, wroteHistory, alerts, newCorrections };
  });
}

type Tx = Parameters<Parameters<Db["transaction"]>[0]>[0];

/**
 * Por quanto tempo o mesmo evento repetido soma no que já está aberto em vez
 * de abrir outro.
 *
 * Um campo que o firmware manda errado não erra uma vez: erra em todo POST,
 * até alguém gravar outro firmware. Foi o caso do `feeder.grains_per_feeding`
 * zerado da 2.0.0 — um evento por POST, 86 mil por dia com o intervalo de
 * 1 s. Agrupado, vira no máximo 24 por dia por campo, e o `repeat_count` (o
 * "×N" da aba Registros) ainda separa um pico isolado de ruído de um defeito
 * que não para.
 */
const REPEAT_WINDOW_MS = 60 * 60 * 1000;

/**
 * Soma 1 ao `repeat_count` do evento do servidor com este código (e, se
 * dado, deste campo) aberto dentro da janela. Falso quando não há nenhum —
 * aí é notícia, e o evento novo é do chamador.
 *
 * Só eventos do servidor: o firmware usa alguns dos mesmos códigos
 * (`temp.implausible`), e somar a repetição de um no evento do outro
 * misturaria quem viu o quê.
 */
async function countRepeatedEvent(
  tx: Tx,
  deviceId: string,
  code: string,
  now: Date,
  field?: string,
): Promise<boolean> {
  const [aberto] = await tx
    .select({ receivedAt: events.receivedAt, id: events.id })
    .from(events)
    .where(
      and(
        eq(events.deviceId, deviceId),
        eq(events.source, "server"),
        eq(events.code, code),
        gte(events.receivedAt, new Date(now.getTime() - REPEAT_WINDOW_MS)),
        field === undefined ? undefined : sql`${events.ctx}->>'field' = ${field}`,
      ),
    )
    .orderBy(desc(events.receivedAt))
    .limit(1);
  if (aberto === undefined) return false;

  await tx
    .update(events)
    .set({ repeatCount: sql`${events.repeatCount} + 1` })
    .where(and(eq(events.receivedAt, aberto.receivedAt), eq(events.id, aberto.id)));
  return true;
}

// ── Temperatura: leitura impossível e faixa habitual ───────────────────────

const plausivel = (c: number) => c >= TEMP_PLAUSIBLE_C.min && c <= TEMP_PLAUSIBLE_C.max;

const foraDoHabitual = (c: number | null): c is number =>
  c !== null && (c < TEMP_USUAL_C.min || c > TEMP_USUAL_C.max);

/** `-48` → `"-48,0"`: o texto é para gente, nos Registros e na planilha. */
const graus = (c: number) => c.toFixed(1).replace(".", ",");

/**
 * Leitura fora de `TEMP_PLAUSIBLE_C` não é temperatura (ver o contrato). O
 * firmware 2.0.2 já a descarta na origem; isto cobre firmware anterior e
 * qualquer regressão. O POST segue como se trouxesse a última leitura boa —
 * nem o banco, nem a saúde, nem o gráfico veem o valor — e o descarte vira
 * um evento, agrupado como as correções: um sensor com defeito erra em todo
 * POST.
 */
async function discardImplausibleTemperature(
  tx: Tx,
  body: TelemetryRequest,
  previous: { tempCelsius: number | null } | undefined,
  now: Date,
): Promise<TelemetryRequest> {
  const c = body.temperature.celsius;
  if (c === null || plausivel(c)) return body;

  if (!(await countRepeatedEvent(tx, body.device_id, "temp.implausible", now))) {
    await insertServerEvent(tx, body.device_id, now, {
      sev: "warn",
      comp: "temp",
      code: "temp.implausible",
      msg: `Leitura impossível descartada: ${graus(c)} °C (a água daqui não sai de ${TEMP_PLAUSIBLE_C.min}–${TEMP_PLAUSIBLE_C.max} °C)`,
      ctx: { received: c },
    });
  }

  const anterior = previous?.tempCelsius ?? null;
  const boa = anterior !== null && plausivel(anterior) ? anterior : null;
  return {
    ...body,
    temperature: {
      ...body.temperature,
      celsius: boa,
      // Sem leitura boa anterior, "sem valor" com `valid` verdadeiro viraria
      // "sensor perdido" na saúde — e o que houve foi uma leitura inválida.
      ...(boa === null ? { valid: false } : {}),
    },
  };
}

const EPISODE_CODE = "temp.out_of_usual_range";
/** Com o episódio aberto, a mensagem ("há 12 min") é reescrita a cada minuto ou num pico novo — não a cada POST. */
const EPISODE_REFRESH_MS = 60_000;
/** Até onde procurar um episódio ainda aberto. */
const EPISODE_LOOKBACK_MS = 30 * 24 * 60 * 60 * 1000;

interface Episode {
  receivedAt: Date;
  id: string;
  direction: "above" | "below";
  since: Date;
  peak: number;
  updatedAt: Date;
}

const limite = (d: Episode["direction"]) => (d === "above" ? TEMP_USUAL_C.max : TEMP_USUAL_C.min);
const lado = (d: Episode["direction"]) => (d === "above" ? "acima" : "abaixo");
const pico = (d: Episode["direction"]) => (d === "above" ? "máx." : "mín.");

/**
 * Água fora de `TEMP_USUAL_C` vira **um** aviso por episódio nos Registros:
 * aberto na primeira leitura fora, atualizado enquanto durar, fechado na
 * primeira de volta — com início, fim, duração e pico. É o que o gráfico deixa
 * de mostrar (o rollup só agrega a faixa habitual).
 *
 * O caso comum, água na faixa agora e no POST anterior, não custa consulta.
 */
async function trackUsualRange(
  tx: Tx,
  deviceId: string,
  celsius: number | null,
  previousCelsius: number | null,
  now: Date,
  tz: string,
): Promise<void> {
  if (!foraDoHabitual(celsius) && !foraDoHabitual(previousCelsius) && previousCelsius !== null) {
    return;
  }
  // Sem leitura agora, nada a decidir: um episódio aberto segue aberto.
  if (celsius === null) return;

  let aberto = await openEpisode(tx, deviceId, now);
  if (!foraDoHabitual(celsius)) {
    if (aberto !== undefined) await closeEpisode(tx, aberto, now, tz);
    return;
  }

  const direction: Episode["direction"] = celsius > TEMP_USUAL_C.max ? "above" : "below";
  if (aberto !== undefined && aberto.direction !== direction) {
    await closeEpisode(tx, aberto, now, tz);
    aberto = undefined;
  }

  if (aberto === undefined) {
    await insertServerEvent(tx, deviceId, now, {
      sev: "warn",
      comp: "temp",
      code: EPISODE_CODE,
      msg: `Água a ${graus(celsius)} °C, ${lado(direction)} da faixa habitual (${TEMP_USUAL_C.min}–${TEMP_USUAL_C.max} °C)`,
      ctx: {
        open: true,
        direction,
        limit: limite(direction),
        since: now.toISOString(),
        peak: celsius,
        updated_at: now.toISOString(),
      },
    });
    return;
  }

  const novoPico = direction === "above" ? Math.max(aberto.peak, celsius) : Math.min(aberto.peak, celsius);
  if (novoPico === aberto.peak && now.getTime() - aberto.updatedAt.getTime() < EPISODE_REFRESH_MS) {
    return;
  }
  await tx
    .update(events)
    .set({
      msg: `Água ${lado(direction)} de ${limite(direction)} °C há ${duracao(now.getTime() - aberto.since.getTime())} (${pico(direction)} ${graus(novoPico)} °C)`,
      ctx: {
        open: true,
        direction,
        limit: limite(direction),
        since: aberto.since.toISOString(),
        peak: novoPico,
        updated_at: now.toISOString(),
      },
    })
    .where(and(eq(events.receivedAt, aberto.receivedAt), eq(events.id, aberto.id)));
}

async function openEpisode(tx: Tx, deviceId: string, now: Date): Promise<Episode | undefined> {
  const [row] = await tx
    .select({ receivedAt: events.receivedAt, id: events.id, ctx: events.ctx })
    .from(events)
    .where(
      and(
        eq(events.deviceId, deviceId),
        eq(events.source, "server"),
        eq(events.code, EPISODE_CODE),
        gte(events.receivedAt, new Date(now.getTime() - EPISODE_LOOKBACK_MS)),
        sql`${events.ctx}->>'open' = 'true'`,
      ),
    )
    .orderBy(desc(events.receivedAt))
    .limit(1);
  const ctx = row?.ctx;
  if (row === undefined || ctx == null) return undefined;

  const since = new Date(String(ctx["since"]));
  const peak = Number(ctx["peak"]);
  if (Number.isNaN(since.getTime()) || !Number.isFinite(peak)) return undefined;
  const updatedAt = new Date(String(ctx["updated_at"]));
  return {
    receivedAt: row.receivedAt,
    id: row.id,
    direction: ctx["direction"] === "below" ? "below" : "above",
    since,
    peak,
    updatedAt: Number.isNaN(updatedAt.getTime()) ? since : updatedAt,
  };
}

async function closeEpisode(tx: Tx, e: Episode, now: Date, tz: string): Promise<void> {
  const ms = now.getTime() - e.since.getTime();
  await tx
    .update(events)
    .set({
      msg: `Água ficou ${lado(e.direction)} de ${limite(e.direction)} °C por ${duracao(ms)}, ${intervalo(e.since, now, tz)} (${pico(e.direction)} ${graus(e.peak)} °C)`,
      ctx: {
        open: false,
        direction: e.direction,
        limit: limite(e.direction),
        since: e.since.toISOString(),
        until: now.toISOString(),
        duration_s: Math.round(ms / 1000),
        peak: e.peak,
        updated_at: now.toISOString(),
      },
    })
    .where(and(eq(events.receivedAt, e.receivedAt), eq(events.id, e.id)));
}

/** `12 min`, `1 h 05 min`, `menos de 1 min`. */
function duracao(ms: number): string {
  const min = Math.floor(ms / 60_000);
  if (min < 1) return "menos de 1 min";
  if (min < 60) return `${min} min`;
  const h = Math.floor(min / 60);
  const resto = min % 60;
  return resto === 0 ? `${h} h` : `${h} h ${String(resto).padStart(2, "0")} min`;
}

/** "das 14:02 às 14:14", com a data quando o episódio atravessa a meia-noite no fuso de exibição. */
function intervalo(de: Date, ate: Date, tz: string): string {
  const dia = new Intl.DateTimeFormat("pt-BR", { timeZone: tz, day: "2-digit", month: "2-digit" });
  const hora = new Intl.DateTimeFormat("pt-BR", {
    timeZone: tz,
    hour: "2-digit",
    minute: "2-digit",
    hourCycle: "h23",
  });
  return dia.format(de) === dia.format(ate)
    ? `das ${hora.format(de)} às ${hora.format(ate)}`
    : `de ${dia.format(de)} ${hora.format(de)} a ${dia.format(ate)} ${hora.format(ate)}`;
}

/**
 * Aplica os `ack` do dispositivo.
 *
 * Devolve o estado desejado da luminária quando um `light.set` foi confirmado
 * com sucesso — é ele que passa a servir de referência para detectar SSR que
 * não obedece.
 */
async function applyAcks(
  tx: Tx,
  body: TelemetryRequest,
  now: Date,
): Promise<boolean | null> {
  if (body.ack.length === 0) return null;

  const ids = body.ack.map((a) => a.id);
  const rows = await tx
    .select()
    .from(commands)
    .where(
      and(eq(commands.deviceId, body.device_id), inArray(commands.id, ids)),
    );

  let lightDesired: boolean | null = null;

  for (const ack of body.ack) {
    const cmd = rows.find((r) => r.id === ack.id);
    if (!cmd) continue;

    /**
     * A confirmação chegou depois de o watchdog marcar `expired` (UPGRADE/04,
     * S8). Não é um reenvio comum: o comando **funcionou**, só a resposta se
     * atrasou. Descartar em silêncio faria a interface continuar mentindo que
     * o comando falhou, quando na verdade `COMMAND_TTL_S` é que está curto
     * demais para as condições reais de rede.
     */
    if (cmd.status === "expired") {
      await insertServerEvent(tx, body.device_id, now, {
        sev: "warn",
        comp: "api",
        code: "cmd.late_ack",
        msg: `Confirmação de ${cmd.action} chegou depois do prazo — o dispositivo executou o comando`,
        ctx: { command_id: ack.id, action: cmd.action, ok: ack.ok },
      });
      continue;
    }

    // Um ack para comando já resolvido de outra forma é reenvio genuíno:
    // ignorar em silêncio.
    if (cmd.status !== "sent" && cmd.status !== "queued") continue;

    await tx
      .update(commands)
      .set({
        status: ack.ok ? "acked" : "rejected",
        settledAt: now,
        errorCode: ack.ok ? null : (ack.code ?? "cmd.rejected"),
      })
      .where(eq(commands.id, ack.id));

    if (!ack.ok) {
      await insertServerEvent(tx, body.device_id, now, {
        sev: "error",
        comp: "api",
        code: "cmd.rejected",
        msg: `O dispositivo recusou ${cmd.action}: ${ack.code ?? "sem motivo informado"}`,
        ctx: { command_id: ack.id, action: cmd.action },
      });
      continue;
    }

    if (cmd.action === "light.set") {
      const on = cmd.payload["on"];
      if (typeof on === "boolean") lightDesired = on;
    }
  }

  return lightDesired;
}

/**
 * Avalia a saúde de cada componente e grava **apenas as transições**.
 *
 * Escrever só na mudança é o que mantém `component_status` minúscula, e é
 * também o que dá sentido ao campo `since`: ele responde "desde quando",
 * não "desde o último POST".
 */
async function recordHealth(
  tx: Tx,
  body: TelemetryRequest,
  now: Date,
  lightDesired: boolean | null,
  alerts: PushMessage[],
): Promise<void> {
  const current = await tx
    .select()
    .from(componentStatus)
    .where(eq(componentStatus.deviceId, body.device_id));

  const previousDetail: Partial<
    Record<Component, Record<string, unknown> | null>
  > = {};
  for (const row of current) previousDetail[row.comp] = row.detail;

  const verdicts = evaluateHealth(body, { previousDetail, lightDesired, now });

  for (const v of verdicts) {
    const before = current.find((r) => r.comp === v.comp);
    const changed = before === undefined || before.status !== v.status;

    if (before !== undefined && !changed) {
      const mudouAlgoQueImporta =
        before.lastCode !== v.code ||
        (before.detail?.["anomaly_since"] ?? null) !== (v.detail?.["anomaly_since"] ?? null);
      const envelheceu = now.getTime() - before.updatedAt.getTime() >= HEALTH_REFRESH_MS;
      if (!mudouAlgoQueImporta && !envelheceu) continue;
    }

    await tx
      .insert(componentStatus)
      .values({
        deviceId: body.device_id,
        comp: v.comp,
        status: v.status,
        since: now,
        lastOkAt: v.status === "ok" ? now : null,
        lastCode: v.code,
        detail: v.detail,
        updatedAt: now,
      })
      .onConflictDoUpdate({
        target: [componentStatus.deviceId, componentStatus.comp],
        set: {
          status: v.status,
          // `since` só se move na transição — é o "desde quando".
          ...(changed ? { since: now } : {}),
          ...(v.status === "ok" ? { lastOkAt: now } : {}),
          lastCode: v.code,
          detail: v.detail,
          updatedAt: now,
        },
      });

    // Transição registrada como evento do servidor: é a trilha auditável de
    // saúde, independente do que o firmware resolveu contar por conta própria.
    //
    // Semear um componente já `ok` no primeiro contato fica em silêncio — não
    // há "antes" para comparar, e "tudo bem" não precisa de log. Mas semear já
    // problemático (o dispositivo ligou com o sensor quebrado) precisa
    // aparecer: sem isto, "não há evento" e "não aconteceu nada" eram
    // indistinguíveis (UPGRADE/04, S10).
    const primeiraVez = before === undefined;
    const recovered = v.status === "ok";

    if (changed && !(primeiraVez && recovered)) {
      const nome = COMPONENT_LABELS[v.comp];
      await insertServerEvent(tx, body.device_id, now, {
        sev: recovered ? "info" : v.status === "degraded" ? "warn" : "error",
        comp: v.comp,
        code: v.code ?? `${v.comp}.recovered`,
        // Texto para humano: esta linha aparece na aba Logs do app, e
        // "wifi: unknown → degraded" é jargão interno vazando para o usuário.
        msg: primeiraVez
          ? `${nome}: ${HEALTH_STATUS_LABELS[v.status].toLowerCase()} desde o primeiro contato`
          : recovered
            ? `${nome} voltou ao normal`
            : `${nome} passou de ${HEALTH_STATUS_LABELS[before.status].toLowerCase()} para ${HEALTH_STATUS_LABELS[v.status].toLowerCase()}`,
        ctx: primeiraVez ? { to: v.status } : { from: before.status, to: v.status },
      });

      /**
       * Notifica só na **entrada** em estado grave, não a cada POST enquanto
       * ele durar. Um sensor que ficou uma semana fora deve tocar o celular
       * uma vez, não vinte mil.
       */
      const eraGrave = before !== undefined && STATUS_QUE_ALERTAM.has(before.status);
      if (STATUS_QUE_ALERTAM.has(v.status) && !eraGrave) {
        alerts.push({
          title: `${nome} com problema`,
          body:
            v.code === "light.state_mismatch"
              ? "A luminária não respondeu ao comando. Verifique o SSR e a lâmpada."
              : `${nome}: ${HEALTH_STATUS_LABELS[v.status].toLowerCase()}. Abra o app para ver o diagnóstico.`,
          // Uma notificação por componente: um problema novo na ventoinha não
          // deve apagar o aviso da luminária, mas o mesmo problema repetido
          // substitui o anterior em vez de empilhar.
          tag: `health-${v.comp}`,
          requireInteraction: true,
        });
      }
    }
  }
}

async function recordDeviceEvents(
  tx: Tx,
  body: TelemetryRequest,
  now: Date,
): Promise<void> {
  if (body.events.length === 0) return;

  await tx.insert(events).values(
    body.events.map((e) => ({
      receivedAt: now,
      deviceId: body.device_id,
      source: "device" as const,
      sev: e.sev,
      comp: e.comp,
      code: e.code,
      msg: e.msg,
      ctx: e.ctx ?? null,
      repeatCount: e.repeat_count,
      deviceTime: e.t ? new Date(e.t) : reconstructDeviceTime(e.ctx, now),
    })),
  );
}

/**
 * Fuso do RTC do dispositivo. Brasília, sem horário de verão desde 2019 — o
 * mesmo `NTP_UTC_OFFSET` do firmware (`config.example.h`).
 */
const DEVICE_TZ_OFFSET_HOURS = -3;

/**
 * Reconstrói `device_time` a partir de `ctx.device_time` (`"HH:MM"`) quando o
 * firmware não manda `t` (UPGRADE/05, S7).
 *
 * O `rtc_manager.cpp` guarda só minutos desde a meia-noite, de propósito: a
 * hora corrente é lida por instrução única, sem tocar o barramento I²C, para
 * evitar o deadlock que um `DateTime` completo exigiria entre o mutex do log e
 * o do I²C. A data, portanto, precisa vir de outro lugar — aqui, do carimbo do
 * servidor.
 *
 * Melhor esforço, não garantia: se o dispositivo represar eventos por horas e
 * cruzar a meia-noite antes do POST, a data reconstruída pode errar por um
 * dia. Ainda assim é estritamente melhor que o `null` que existia antes.
 */
function reconstructDeviceTime(
  ctx: Record<string, unknown> | null | undefined,
  receivedAt: Date,
): Date | null {
  const hhmm = ctx?.["device_time"];
  if (typeof hhmm !== "string") return null;

  const m = /^([01]\d|2[0-3]):([0-5]\d)$/.exec(hhmm);
  if (!m?.[1] || !m[2]) return null;
  const hour = Number(m[1]);
  const minute = Number(m[2]);

  const localCalendarDay = new Date(
    receivedAt.getTime() + DEVICE_TZ_OFFSET_HOURS * 3_600_000,
  );
  return new Date(
    Date.UTC(
      localCalendarDay.getUTCFullYear(),
      localCalendarDay.getUTCMonth(),
      localCalendarDay.getUTCDate(),
      hour - DEVICE_TZ_OFFSET_HOURS,
      minute,
    ),
  );
}

async function insertServerEvent(
  tx: Tx,
  deviceId: string,
  now: Date,
  e: {
    sev: "debug" | "info" | "warn" | "error" | "fatal";
    comp: Component;
    code: string;
    msg: string;
    ctx?: Record<string, unknown>;
  },
): Promise<void> {
  await tx.insert(events).values({
    receivedAt: now,
    deviceId,
    source: "server",
    sev: e.sev,
    comp: e.comp,
    code: e.code,
    msg: e.msg,
    ctx: e.ctx ?? null,
    repeatCount: 1,
    deviceTime: null,
  });
}

/**
 * Ordem de gravidade dos status — o veredito de um relatório é o **pior**
 * check, não a média. Um sensor ausente não é compensado por cinco
 * componentes saudáveis.
 */
const STATUS_SEVERITY: Record<HealthStatus, number> = {
  ok: 0,
  unknown: 1,
  degraded: 2,
  missing: 3,
  fault: 4,
};

/**
 * Persiste o relatório de autodiagnóstico e registra o desfecho como evento.
 *
 * O evento importa tanto quanto a tabela: é ele que faz um diagnóstico ruim
 * aparecer na aba Logs junto com o resto da história, em vez de ficar só numa
 * tela que alguém precisa lembrar de abrir.
 */
async function recordDiagnostic(
  tx: Tx,
  body: TelemetryRequest,
  now: Date,
): Promise<DiagnosticColumns | undefined> {
  const report = body.diagnostic;
  if (report === undefined) return undefined;

  const overall = report.checks.reduce<HealthStatus>(
    (pior, c) => (STATUS_SEVERITY[c.status] > STATUS_SEVERITY[pior] ? c.status : pior),
    "ok",
  );

  /**
   * `unknown` **não** é problema: é "não deu para verificar".
   *
   * A luminária cai nesse caso em toda execução — o SSR não tem retorno, e o
   * firmware é honesto ao dizer que não sabe em vez de fingir que está `ok`.
   * Contá-la como problema faria todo diagnóstico terminar em alerta, e um
   * alarme que sempre dispara é um alarme que se aprende a ignorar.
   */
  const problemas = report.checks.filter(
    (c) => c.status !== "ok" && c.status !== "unknown",
  );
  const saudavel = problemas.length === 0;

  await insertServerEvent(tx, body.device_id, now, {
    sev: saudavel ? "info" : "warn",
    comp: "system",
    code: saudavel ? "system.diagnostic_ran" : "system.diagnostic_found_problem",
    msg: saudavel
      ? `Autodiagnóstico concluído em ${report.duration_ms} ms: ${report.checks.length} verificações, nenhum problema`
      : `Autodiagnóstico encontrou problema em: ${problemas.map((c) => COMPONENT_LABELS[c.comp]).join(", ")}`,
    ctx: {
      command_id: report.command_id ?? null,
      duration_ms: report.duration_ms,
      checks: report.checks.length,
      problems: problemas.length,
    },
  });

  return {
    diagnosticRanAt: now,
    diagnosticCommandId: report.command_id ?? null,
    diagnosticDurationMs: report.duration_ms,
    diagnosticOverall: overall,
    diagnosticChecks: report.checks as unknown as Record<string, unknown>[],
  };
}

/** Colunas do relatório de autodiagnóstico, prontas para o upsert. */
type DiagnosticColumns = {
  diagnosticRanAt: Date;
  diagnosticCommandId: number | null;
  diagnosticDurationMs: number;
  diagnosticOverall: HealthStatus;
  diagnosticChecks: Record<string, unknown>[];
};

async function upsertState(
  tx: Tx,
  body: TelemetryRequest,
  now: Date,
  extra: {
    lightDesired: boolean | null;
    lastHistoryAt: Date | null;
    diagnostic?: DiagnosticColumns | undefined;
    feederColumns: {
      feederConnected: boolean;
      feederAutoEnabled: boolean | null;
      feederHour1: number | null;
      feederHour2: number | null;
      feederGrainsPerFeeding: number | null;
      feederLastFeedAt: Date | null;
      feederLastFeedRequested: number | null;
      feederLastFeedConfirmed: number | null;
      feederLastFeedOk: boolean | null;
    };
  },
): Promise<void> {
  const row = {
    deviceId: body.device_id,
    lightOn: body.light.on,
    lightSource: body.light.source,
    lightDesired: extra.lightDesired,
    tempCelsius: body.temperature.celsius,
    tempAvailable: body.temperature.available,
    tempValid: body.temperature.valid,
    tempAgeMs: body.temperature.age_ms,
    fanOn: body.fan.on,
    fanSpeedPercent: body.fan.speed_percent,
    fanRpm: body.fan.rpm,
    fanMode: body.fan.mode,
    rtcAvailable: body.rtc.available,
    rtcTime: body.rtc.time,
    rtcLostPower: body.rtc.lost_power,
    wifiRssi: body.wifi.rssi,
    wifiIp: body.wifi.ip,
    wifiReconnects: body.wifi.reconnects,
    uptimeMs: body.uptime_ms,
    configVersion: body.config_version,
    lastBootId: body.boot_id,
    lastSeq: body.seq,
    updatedAt: now,
    lastHistoryAt: extra.lastHistoryAt,
    deviceTime: body.device_time ? new Date(body.device_time) : null,

    // Diagnóstico do controlador (UPGRADE/04, S1) — sempre o último valor
    // conhecido. `resetReason` só muda uma vez por boot; o histórico dele mora
    // no evento `system.boot`, não numa coluna repetida a cada linha.
    resetReason: body.diag?.reset_reason ?? null,
    freeHeap: body.diag?.free_heap ?? null,
    minFreeHeap: body.diag?.min_free_heap ?? null,
    maxAllocHeap: body.diag?.max_alloc_heap ?? null,
    postLatencyMs: body.diag?.post_latency_ms ?? null,
    apiFailures: body.diag?.api_failures ?? 0,
    potRawAdc: body.diag?.pot_raw_adc ?? null,
    buttonPressed: body.diag?.button_pressed ?? false,
    tachPulsesRaw: body.diag?.tach_pulses_raw ?? null,
    netTaskStackHwm: body.diag?.net_task_stack_hwm ?? null,
    bootCount: body.diag?.boot_count ?? null,
    nvsFailures: body.diag?.nvs_failures ?? 0,
    otaLastResult: body.diag?.ota_last_result ?? "none",
    eventsDropped: body.diag?.events_dropped ?? 0,

    ...extra.feederColumns,

    // O relatório só viaja no POST seguinte ao `device.diagnose`; nos demais
    // ciclos as colunas não são tocadas, preservando o último que houve.
    ...(extra.diagnostic ?? {}),
  };

  const { deviceId: _ignored, ...updatable } = row;

  await tx
    .insert(deviceState)
    .values(row)
    .onConflictDoUpdate({ target: deviceState.deviceId, set: updatable });
}

/** Lê a configuração do dispositivo, criando o padrão no primeiro contato. */
async function loadConfig(
  tx: Tx,
  deviceId: string,
  now: Date,
): Promise<{ config: DeviceConfig; configVersion: number }> {
  const [row] = await tx
    .select()
    .from(settings)
    .where(eq(settings.deviceId, deviceId))
    .limit(1);

  if (row === undefined) {
    await tx
      .insert(settings)
      .values({
        deviceId,
        config: DEFAULT_CONFIG,
        configVersion: 1,
        updatedAt: now,
      })
      .onConflictDoNothing();
    return { config: DEFAULT_CONFIG, configVersion: 1 };
  }

  const parsed = deviceConfigSchema.safeParse(row.config);
  // Configuração corrompida no banco não pode derrubar o ingest nem deixar o
  // aquário sem regra: cai no padrão e segue.
  return {
    config: parsed.success ? parsed.data : DEFAULT_CONFIG,
    configVersion: row.configVersion,
  };
}

/**
 * Teto de idade para entrega (UPGRADE/04, S4). Um dispositivo que volta de
 * meia hora offline não deve executar uma fila de decisões já obsoletas — a
 * regra de `superseded` protege comandos do **mesmo alvo**, mas não protege
 * alvos diferentes: um `device.reboot` de ontem ainda dispararia sem isto.
 */
const MAX_QUEUED_AGE_MS = 15 * 60 * 1000;

/**
 * Devolve os comandos pendentes e marca os novos como entregues.
 *
 * Um comando **não sai da fila ao ser entregue** — só quando o dispositivo o
 * confirma no `ack`. Se a resposta se perder, ele é reentregue no POST
 * seguinte, que é exatamente o que o contrato exige.
 */
async function takePendingCommands(
  tx: Tx,
  deviceId: string,
  now: Date,
): Promise<PendingCommand[]> {
  const rows = await tx
    .select()
    .from(commands)
    .where(
      and(
        eq(commands.deviceId, deviceId),
        inArray(commands.status, ["queued", "sent"]),
      ),
    )
    .orderBy(asc(commands.createdAt))
    .limit(MAX_COMMANDS_PER_RESPONSE);

  const stale = rows.filter((r) => now.getTime() - r.createdAt.getTime() > MAX_QUEUED_AGE_MS);
  const deliverable = rows.filter((r) => !stale.includes(r));

  if (stale.length > 0) {
    await tx
      .update(commands)
      .set({ status: "expired", settledAt: now, errorCode: "cmd.stale_discarded" })
      .where(inArray(commands.id, stale.map((r) => r.id)));

    for (const cmd of stale) {
      await insertServerEvent(tx, deviceId, now, {
        sev: "warn",
        comp: "api",
        code: "cmd.stale_discarded",
        msg: `${cmd.action} descartado sem executar — mais de ${Math.round(MAX_QUEUED_AGE_MS / 60000)} min na fila`,
        ctx: { command_id: cmd.id, action: cmd.action },
      });
    }
  }

  const novos = deliverable.filter((r) => r.status === "queued").map((r) => r.id);
  if (novos.length > 0) {
    await tx
      .update(commands)
      .set({ status: "sent", sentAt: now, errorCode: null })
      .where(inArray(commands.id, novos));
  }

  return deliverable.map(
    (r) => ({ id: r.id, action: r.action, ...r.payload }) as PendingCommand,
  );
}
