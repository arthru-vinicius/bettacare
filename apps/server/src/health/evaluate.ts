import type {
  Component,
  HealthStatus,
  TelemetryRequest,
} from "@bettacare/contract";

/**
 * As regras que transformam telemetria crua em veredito de saúde.
 *
 * Função pura de propósito: não toca banco, não lê relógio, não registra nada.
 * Toda a dificuldade deste módulo está nas regras, e regras puras são as
 * únicas que dá para testar sem subir infraestrutura.
 */

/** Leitura mais velha que isto deixa de valer como estado atual. */
const TEMP_STALE_MS = 30_000;
/** Abaixo disto o Wi-Fi ainda conecta, mas cai com frequência. */
const WIFI_WEAK_RSSI = -80;
/**
 * Por quanto tempo uma anomalia precisa persistir para virar `fault`.
 *
 * Medido em **tempo**, não em POSTs seguidos (UPGRADE/07). A versão anterior
 * contava dois "strikes", que com POST a cada 3 s davam ~6 s; com o intervalo
 * ajustável para 1 s, dois strikes virariam 2 s — menos que a janela de 2 s em
 * que o firmware conta os pulsos do tacômetro, então toda ventoinha recém-
 * -ligada viraria "travada" (e push) antes da primeira contagem chegar.
 */
const ANOMALY_TO_FAULT_MS = 5_000;
/** O mesmo limiar que o firmware usa em `light_button_stuck()`. */
const BUTTON_STUCK_MS = 30_000;
/** Topo do curso do potenciômetro — ver UPGRADE/02 §8 sobre saturação do ADC do ESP32. */
const POT_SATURATED_ADC = 4090;
/**
 * Motivos de reset que indicam falha, não operação normal. `poweron` e `sw`
 * (reinício comandado) ficam de fora de propósito — são esperados.
 */
const UNCLEAN_RESETS = new Set([
  "brownout",
  "panic",
  "int_wdt",
  "task_wdt",
  "wdt",
  "cpu_lockup",
  "pwr_glitch",
]);
/**
 * Abaixo disto o ESP32 ainda funciona, mas uma alocação grande — o buffer do
 * POST, um handshake — passa a poder falhar. O firmware sobe com ~270 KB
 * livres, então 40 KB é sinal de que algo consumiu quase tudo.
 */
const LOW_HEAP_BYTES = 40_000;

export interface HealthVerdict {
  comp: Component;
  status: HealthStatus;
  /** O código que justifica o status. Nulo quando está `ok`. */
  code: string | null;
  detail: Record<string, unknown> | null;
}

/** O que a avaliação precisa saber do ciclo anterior. */
export interface HealthContext {
  /** `detail` gravado na última avaliação, por componente. */
  previousDetail: Partial<Record<Component, Record<string, unknown> | null>>;
  /** Último estado que o servidor mandou a luminária assumir. */
  lightDesired: boolean | null;
  /** Instante deste POST, pelo relógio do servidor. */
  now: Date;
}

/**
 * Há quanto tempo uma anomalia persiste.
 *
 * O início fica no próprio `detail` do componente (`anomaly_since`, ISO), que o
 * ingest regrava na transição — é o que torna a regra independente do
 * intervalo de telemetria e de POSTs perdidos no meio do caminho.
 */
function anomalyOf(
  ctx: HealthContext,
  comp: Component,
  condition: boolean,
): { since: string | null; ms: number } {
  if (!condition) return { since: null, ms: 0 };
  const previous = ctx.previousDetail[comp]?.["anomaly_since"];
  const since =
    typeof previous === "string" && !Number.isNaN(Date.parse(previous))
      ? previous
      : ctx.now.toISOString();
  return { since, ms: Math.max(0, ctx.now.getTime() - Date.parse(since)) };
}

export function evaluateHealth(
  t: TelemetryRequest,
  ctx: HealthContext,
): HealthVerdict[] {
  return [
    evaluateTemp(t),
    evaluateRtc(t),
    evaluateFan(t, ctx),
    evaluateLight(t, ctx),
    evaluateWifi(t),
    evaluateApi(t),
    evaluateButton(t, ctx),
    evaluatePot(t),
    evaluateNvs(t),
    evaluateOta(t),
    evaluateSystem(t),
    // Os avaliadores que dependem de `diag` voltam `null` para um firmware
    // que ainda não o manda — sem o dado, não há veredito honesto a dar, e o
    // componente simplesmente não aparece na aba Saúde (em vez de fingir
    // "sem informação" para sempre).
  ].filter((v): v is HealthVerdict => v !== null);
}

function evaluateTemp(t: TelemetryRequest): HealthVerdict {
  const { celsius, available, valid, age_ms } = t.temperature;
  const detail = { celsius, age_ms };

  if (!available) {
    return { comp: "temp", status: "missing", code: "temp.sensor_lost", detail };
  }
  if (!valid) {
    return { comp: "temp", status: "degraded", code: "temp.crc_error", detail };
  }
  if (celsius === null) {
    return { comp: "temp", status: "missing", code: "temp.sensor_lost", detail };
  }
  // Sem idade não há leitura válida — o sensor está no barramento mas nunca
  // entregou nada aproveitável, que é diferente de ter sumido.
  if (age_ms === null) {
    return { comp: "temp", status: "degraded", code: "temp.stale", detail };
  }
  if (age_ms > TEMP_STALE_MS) {
    return { comp: "temp", status: "degraded", code: "temp.stale", detail };
  }
  return { comp: "temp", status: "ok", code: null, detail };
}

function evaluateRtc(t: TelemetryRequest): HealthVerdict {
  const detail = { time: t.rtc.time };

  if (!t.rtc.available) {
    return { comp: "rtc", status: "missing", code: "rtc.missing", detail };
  }
  if (t.rtc.lost_power) {
    return { comp: "rtc", status: "degraded", code: "rtc.lost_power", detail };
  }
  return { comp: "rtc", status: "ok", code: null, detail };
}

function evaluateFan(t: TelemetryRequest, ctx: HealthContext): HealthVerdict {
  const { speed_percent, rpm, mode } = t.fan;

  /**
   * O tacômetro é a única realimentação real do sistema inteiro. PWM acima de
   * zero com rotação zerada significa fisicamente uma coisa só: a ventoinha
   * não está girando — cabo solto, rolamento travado ou fonte caída.
   */
  const stalled = speed_percent > 0 && rpm === 0;
  const anomaly = anomalyOf(ctx, "fan", stalled);
  const detail = { rpm, speed_percent, mode, anomaly_since: anomaly.since };

  if (anomaly.ms >= ANOMALY_TO_FAULT_MS) {
    return { comp: "fan", status: "fault", code: "fan.tach_stalled", detail };
  }
  if (mode === "failsafe") {
    return { comp: "fan", status: "degraded", code: "fan.failsafe", detail };
  }
  return { comp: "fan", status: "ok", code: null, detail };
}

function evaluateLight(t: TelemetryRequest, ctx: HealthContext): HealthVerdict {
  /**
   * O SSR não tem retorno: o firmware reporta o GPIO que escreveu, não o que a
   * lâmpada fez. A divergência que sobra para detectar é a de o dispositivo
   * dizer que a última mudança veio do comando e, ainda assim, o estado não
   * ser o comandado — firmware fora do eixo.
   *
   * Só vale com `source === "command"` (UPGRADE/07). Antes valia sempre, e o
   * "desejado" de um comando antigo ficava de pé para sempre: apertar o botão
   * físico, ou a automação virar o período, contava como falha da luminária —
   * 300 `light.state_mismatch` e pushes "a luminária não respondeu" num só dia
   * de bancada, todos falsos.
   */
  const mismatch =
    t.light.source === "command" &&
    ctx.lightDesired !== null &&
    t.light.on !== ctx.lightDesired;
  const anomaly = anomalyOf(ctx, "light", mismatch);
  const detail = {
    on: t.light.on,
    desired: ctx.lightDesired,
    source: t.light.source,
    anomaly_since: anomaly.since,
  };

  if (anomaly.ms >= ANOMALY_TO_FAULT_MS) {
    return {
      comp: "light",
      status: "fault",
      code: "light.state_mismatch",
      detail,
    };
  }
  return { comp: "light", status: "ok", code: null, detail };
}

function evaluateWifi(t: TelemetryRequest): HealthVerdict {
  const detail = { rssi: t.wifi.rssi, ip: t.wifi.ip, reconnects: t.wifi.reconnects };

  // `null` é "sem leitura no instante do snapshot" (UPGRADE/03, F13), não
  // "sinal ruim" — tratar como degraded aqui inflaria falsos alarmes a partir
  // de uma janela de poucos milissegundos sem associação.
  if (t.wifi.rssi === null) {
    return { comp: "wifi", status: "unknown", code: null, detail };
  }
  if (t.wifi.rssi < WIFI_WEAK_RSSI) {
    return { comp: "wifi", status: "degraded", code: "wifi.weak_signal", detail };
  }
  return { comp: "wifi", status: "ok", code: null, detail };
}

/**
 * Substitui a tautologia anterior ("chegou POST, então tudo bem") por uma
 * regra real (UPGRADE/04, S3). `api_failures` já existe no firmware desde a
 * reescrita — só faltava sair no corpo.
 */
function evaluateApi(t: TelemetryRequest): HealthVerdict {
  const failures = t.diag?.api_failures ?? 0;
  const detail = { failures, post_latency_ms: t.diag?.post_latency_ms ?? null };

  /**
   * Este POST chegou, então a comunicação funciona **agora** — por isso não
   * há nível `fault` aqui: "falhando há mais de 60 s" contradiz o fato de que
   * a mensagem acabou de chegar. O que `api_failures` revela é outra coisa:
   * quantas tentativas precederam este sucesso — o sinal precoce de rede
   * degradando antes de cair de vez.
   */
  if (failures > 0) {
    return { comp: "api", status: "degraded", code: "api.post_failed", detail };
  }
  return { comp: "api", status: "ok", code: null, detail };
}

/**
 * `button` entrou na aba Saúde nesta rodada (UPGRADE/04, S2) — era invisível
 * mesmo sendo uma entrada física que também falha. O firmware já detecta e
 * emite `button.stuck` sozinho; isto é a corroboração do lado do servidor.
 */
function evaluateButton(t: TelemetryRequest, ctx: HealthContext): HealthVerdict | null {
  if (t.diag === undefined) return null;

  const pressed = t.diag.button_pressed;
  const anomaly = anomalyOf(ctx, "button", pressed);
  const detail = { pressed, anomaly_since: anomaly.since };

  if (anomaly.ms >= BUTTON_STUCK_MS) {
    return { comp: "button", status: "degraded", code: "button.stuck", detail };
  }
  return { comp: "button", status: "ok", code: null, detail };
}

/**
 * `pot` era o exemplo citado em `docs/arquitetura-observabilidade.md` como
 * "hoje invisível" — e continuava sendo, porque nenhum evento avaliava a
 * leitura crua do ADC (UPGRADE/04, S2).
 */
function evaluatePot(t: TelemetryRequest): HealthVerdict | null {
  const adc = t.diag?.pot_raw_adc;
  if (adc === undefined || adc === null) return null;

  const detail = { adc };
  if (adc >= POT_SATURATED_ADC) {
    return { comp: "pot", status: "degraded", code: "pot.out_of_range", detail };
  }
  return { comp: "pot", status: "ok", code: null, detail };
}

/**
 * `nvs` emitia quatro eventos e não tinha saúde nenhuma. Uma falha de escrita
 * significa que a configuração vale para esta sessão mas **não sobrevive ao
 * próximo reboot** — precisa aparecer na aba Saúde, não só passar no log.
 */
function evaluateNvs(t: TelemetryRequest): HealthVerdict | null {
  if (t.diag === undefined) return null;

  const failures = t.diag.nvs_failures;
  const detail = { failures };

  // Qualquer falha já é degradação real: a flash ou responde ou não responde,
  // não existe "meia escrita" tolerável.
  if (failures > 0) {
    return { comp: "nvs", status: "degraded", code: "nvs.write_failed", detail };
  }
  return { comp: "nvs", status: "ok", code: null, detail };
}

/**
 * `ota` era componente morto — declarado no contrato, nunca emitido. Os
 * callbacks do ElegantOTA só imprimiam no Serial, então **uma atualização que
 * falhava era invisível** — e OTA é justamente a via de recuperação remota.
 */
function evaluateOta(t: TelemetryRequest): HealthVerdict | null {
  if (t.diag === undefined) return null;

  const resultado = t.diag.ota_last_result;
  const detail = { last_result: resultado };

  if (resultado === "failed") {
    return { comp: "ota", status: "fault", code: "ota.failed", detail };
  }
  // `none` (nunca houve OTA) é tão saudável quanto `ok`: a ausência de
  // atualização não é defeito.
  return { comp: "ota", status: "ok", code: null, detail };
}

/**
 * Saúde do próprio controlador, derivada do que `diag` já traz.
 *
 * Note o que **não** dá para avaliar aqui: `system.task_failed` (a task de
 * rede não subiu) é estruturalmente inalcançável, porque é essa mesma task
 * que faz o POST — se ela não subiu, não há telemetria nenhuma chegando. Esse
 * caso aparece como dispositivo offline, e é o watchdog do servidor que o
 * detecta.
 */
function evaluateSystem(t: TelemetryRequest): HealthVerdict | null {
  if (t.diag === undefined) return null;

  const { reset_reason, free_heap, max_alloc_heap, events_dropped } = t.diag;
  const detail = {
    reset_reason: reset_reason ?? null,
    free_heap: free_heap ?? null,
    max_alloc_heap: max_alloc_heap ?? null,
    events_dropped,
  };

  // Reinício não comandado é o sinal mais forte que existe: aponta para
  // alimentação (brownout) ou para software travado (watchdog). Ver
  // docs/pinagem-e-montagem-esp32.md §1.
  if (reset_reason != null && UNCLEAN_RESETS.has(reset_reason)) {
    return { comp: "system", status: "fault", code: "system.unclean_reset", detail };
  }
  if (free_heap != null && free_heap < LOW_HEAP_BYTES) {
    return { comp: "system", status: "degraded", code: "system.low_memory", detail };
  }
  // Perder evento por estouro de buffer é perder diagnóstico — degradação da
  // própria capacidade de investigar, que merece aparecer.
  if (events_dropped > 0) {
    return {
      comp: "system",
      status: "degraded",
      code: "system.event_overflow",
      detail,
    };
  }
  return { comp: "system", status: "ok", code: null, detail };
}

/**
 * Decide se este POST merece uma linha de histórico.
 *
 * O dispositivo fala a cada 3 s, mas o banco só ganha linha quando algo muda
 * ou quando passa o intervalo de heartbeat — é o que mantém ~2.000 linhas/dia
 * em vez de 28.800.
 *
 * `rpm` e `rssi` ficam **de fora** da comparação de propósito: os dois oscilam
 * a cada leitura e, incluídos, todo POST viraria uma mudança, o que anularia a
 * economia inteira. Eles são gravados na linha, só não a disparam.
 */
export function shouldWriteHistory(
  t: TelemetryRequest,
  previous: {
    lightOn: boolean;
    fanOn: boolean;
    fanSpeedPercent: number;
    fanMode: string;
    tempCelsius: number | null;
    tempAvailable: boolean;
    lastHistoryAt: Date | null;
  } | null,
  now: Date,
  heartbeatMs: number,
): boolean {
  if (previous === null) return true;
  if (previous.lastHistoryAt === null) return true;

  if (now.getTime() - previous.lastHistoryAt.getTime() >= heartbeatMs) {
    return true;
  }

  if (t.light.on !== previous.lightOn) return true;
  if (t.fan.on !== previous.fanOn) return true;
  if (t.fan.speed_percent !== previous.fanSpeedPercent) return true;
  if (t.fan.mode !== previous.fanMode) return true;
  if (t.temperature.available !== previous.tempAvailable) return true;

  // Décimos de grau: é a resolução que o DS18B20 entrega e a que a interface
  // mostra. Comparar float cru gravaria linha a cada ruído do último bit.
  const before = previous.tempCelsius === null ? null : Math.round(previous.tempCelsius * 10);
  const after =
    t.temperature.celsius === null ? null : Math.round(t.temperature.celsius * 10);
  if (before !== after) return true;

  return false;
}
