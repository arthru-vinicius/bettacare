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
 * Quantos POSTs seguidos uma anomalia precisa persistir para virar `fault`.
 * Com POST a cada 3 s, dois strikes são ~6 s — tempo suficiente para descartar
 * a leitura isolada e curto o bastante para não esconder um defeito real.
 */
const STRIKES_TO_FAULT = 2;

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
}

function strikesOf(
  ctx: HealthContext,
  comp: Component,
  condition: boolean,
): number {
  if (!condition) return 0;
  const previous = ctx.previousDetail[comp]?.["strikes"];
  return (typeof previous === "number" ? previous : 0) + 1;
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
    // Se este POST chegou, a comunicação está funcionando. É tautológico, mas
    // é o que faz a aba Saúde mostrar a linha verde em vez de "sem informação".
    { comp: "api", status: "ok", code: null, detail: null },
  ];
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
  const strikes = strikesOf(ctx, "fan", stalled);
  const detail = { rpm, speed_percent, mode, strikes };

  if (strikes >= STRIKES_TO_FAULT) {
    return { comp: "fan", status: "fault", code: "fan.tach_stalled", detail };
  }
  if (mode === "failsafe") {
    return { comp: "fan", status: "degraded", code: "fan.failsafe", detail };
  }
  return { comp: "fan", status: "ok", code: null, detail };
}

function evaluateLight(t: TelemetryRequest, ctx: HealthContext): HealthVerdict {
  /**
   * O SSR não tem retorno, então a única evidência possível de falha é a
   * divergência entre o que foi comandado e o que o firmware reporta. Não
   * afirma *qual* é o defeito, mas afirma com segurança que existe um.
   */
  const mismatch =
    ctx.lightDesired !== null && t.light.on !== ctx.lightDesired;
  const strikes = strikesOf(ctx, "light", mismatch);
  const detail = {
    on: t.light.on,
    desired: ctx.lightDesired,
    source: t.light.source,
    strikes,
  };

  if (strikes >= STRIKES_TO_FAULT) {
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

  if (t.wifi.rssi < WIFI_WEAK_RSSI) {
    return { comp: "wifi", status: "degraded", code: "wifi.weak_signal", detail };
  }
  return { comp: "wifi", status: "ok", code: null, detail };
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
