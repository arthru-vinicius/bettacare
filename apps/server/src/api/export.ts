import {
  COMPONENT_LABELS,
  describeEventCode,
  type Component,
  type Severity,
} from "@bettacare/contract";
import { and, asc, eq, gte, lt, sql } from "drizzle-orm";

import type { Db } from "../db/client.js";
import { events, telemetry } from "../db/schema.js";
import { eventFilterWhere, type EventFilterBase } from "./queries.js";

/**
 * Exportação para planilha — o "Relatórios" que o usuário pediu: baixar os
 * registros do aquário e do servidor, ou as medições, de um período escolhido.
 *
 * CSV no formato que o Excel em português abre com dois cliques: separador
 * `;`, vírgula decimal, BOM de UTF-8 (sem ele o Excel lê os acentos como
 * Latin-1) e datas no fuso de exibição. O Google Planilhas lê igual.
 *
 * Gerado em lotes e enviado em fluxo: o container tem teto de 256 MB, e uma
 * planilha de um ano inteiro não pode caber inteira na memória para sair.
 */

/** Linhas por consulta ao banco. */
const LOTE = 1000;
/** Teto de linhas por planilha — um ano de eventos de bancada cabe com folga. */
const MAX_LINHAS = 200_000;
/** Período máximo de uma exportação. */
const MAX_DIAS = 400;

const NIVEL: Record<Severity, string> = {
  debug: "Depuração",
  info: "Informação",
  warn: "Aviso",
  error: "Erro",
  fatal: "Crítico",
};

const MODO_VENTOINHA: Record<string, string> = {
  auto: "Automático",
  manual: "Manual",
  manual_off: "Desligada manualmente",
  failsafe: "Segurança",
};

export interface Periodo {
  from: Date;
  to: Date;
}

/**
 * Lê `from`/`to` (ISO) da query. Sem `to`, agora; sem `from`, sete dias antes
 * de `to`. Nulo se as datas forem inválidas, invertidas ou longas demais.
 */
export function parsePeriodo(fromRaw?: string, toRaw?: string): Periodo | null {
  const to = toRaw ? new Date(toRaw) : new Date();
  const from = fromRaw ? new Date(fromRaw) : new Date(to.getTime() - 7 * 86_400_000);
  if (Number.isNaN(from.getTime()) || Number.isNaN(to.getTime())) return null;
  if (from >= to) return null;
  if (to.getTime() - from.getTime() > MAX_DIAS * 86_400_000) return null;
  return { from, to };
}

/** `bettacare-registros-2026-09-25-a-2026-10-02.csv`, no fuso de exibição. */
export function nomeDoArquivo(tipo: string, p: Periodo, tz: string): string {
  const dia = (d: Date) => new Intl.DateTimeFormat("en-CA", { timeZone: tz }).format(d);
  // O fim é exclusivo: o último dia coberto é o anterior a `to`, se `to` cair
  // na meia-noite exata — que é o caso de "até hoje" vindo da interface.
  const ultimo = new Date(p.to.getTime() - 1);
  return `bettacare-${tipo}-${dia(p.from)}-a-${dia(ultimo)}.csv`;
}

export function eventosCsv(
  db: Db,
  filtro: EventFilterBase & Periodo,
  tz: string,
): ReadableStream<Uint8Array> {
  const data = formatadorData(tz);

  async function* linhas(): AsyncGenerator<string> {
    yield "﻿";
    yield linha([
      "Data e hora",
      "Nível",
      "Origem",
      "Equipamento",
      "Evento",
      "Mensagem",
      "Repetições",
      "Hora no aquário",
      "Código",
      "Data e hora (UTC)",
    ]);

    let cursor: { at: Date; id: string } | null = null;
    let total = 0;

    while (total < MAX_LINHAS) {
      const where = eventFilterWhere(filtro);
      if (cursor) {
        where.push(sql`(${events.receivedAt}, ${events.id}) > (${cursor.at}::timestamptz, ${cursor.id})`);
      }
      const lote = await db
        .select()
        .from(events)
        .where(and(...where))
        .orderBy(asc(events.receivedAt), asc(events.id))
        .limit(LOTE);

      for (const e of lote) {
        const horaAquario = e.ctx?.["device_time"];
        yield linha([
          data(e.receivedAt),
          NIVEL[e.sev],
          e.source === "device" ? "Aquário" : "Servidor",
          COMPONENT_LABELS[e.comp as Component] ?? e.comp,
          texto(describeEventCode(e.code)?.label ?? e.code),
          texto(e.msg),
          e.repeatCount,
          typeof horaAquario === "string" ? horaAquario : "",
          texto(e.code),
          e.receivedAt.toISOString(),
        ]);
      }

      total += lote.length;
      const ultimo = lote.at(-1);
      if (lote.length < LOTE || ultimo === undefined) return;
      cursor = { at: ultimo.receivedAt, id: ultimo.id };
    }
  }

  return fluxo(linhas());
}

export function medicoesCsv(
  db: Db,
  filtro: { deviceId: string } & Periodo,
  tz: string,
): ReadableStream<Uint8Array> {
  const data = formatadorData(tz);

  async function* linhas(): AsyncGenerator<string> {
    yield "﻿";
    yield linha([
      "Data e hora",
      "Temperatura (°C)",
      "Leitura válida",
      "Luminária",
      "Ventoinha",
      "Velocidade (%)",
      "Rotação (rpm)",
      "Modo da ventoinha",
      "Sinal Wi-Fi (dBm)",
      "Memória livre (KB)",
      "Data e hora (UTC)",
    ]);

    let cursor: { at: Date; bootId: number; seq: number } | null = null;
    let total = 0;

    while (total < MAX_LINHAS) {
      const where = [
        eq(telemetry.deviceId, filtro.deviceId),
        gte(telemetry.receivedAt, filtro.from),
        lt(telemetry.receivedAt, filtro.to),
      ];
      if (cursor) {
        where.push(
          sql`(${telemetry.receivedAt}, ${telemetry.bootId}, ${telemetry.seq}) > (${cursor.at}::timestamptz, ${cursor.bootId}, ${cursor.seq})`,
        );
      }
      const lote = await db
        .select()
        .from(telemetry)
        .where(and(...where))
        .orderBy(asc(telemetry.receivedAt), asc(telemetry.bootId), asc(telemetry.seq))
        .limit(LOTE);

      for (const t of lote) {
        yield linha([
          data(t.receivedAt),
          decimal(t.tempCelsius, 2),
          t.tempValid ? "Sim" : "Não",
          t.lightOn ? "Acesa" : "Apagada",
          t.fanOn ? "Ligada" : "Desligada",
          t.fanSpeedPercent,
          t.fanRpm,
          MODO_VENTOINHA[t.fanMode] ?? t.fanMode,
          t.wifiRssi,
          t.freeHeap === null ? null : Math.round(t.freeHeap / 1024),
          t.receivedAt.toISOString(),
        ]);
      }

      total += lote.length;
      const ultimo = lote.at(-1);
      if (lote.length < LOTE || ultimo === undefined) return;
      cursor = { at: ultimo.receivedAt, bootId: ultimo.bootId, seq: ultimo.seq };
    }
  }

  return fluxo(linhas());
}

// ── CSV ──────────────────────────────────────────────────────────────────────

/** Valor já tratado como texto (não passa pela proteção de fórmula de novo). */
type Celula = string | number | null | undefined | { texto: string };

function texto(s: string): { texto: string } {
  return { texto: s };
}

function linha(celulas: Celula[]): string {
  return `${celulas.map(celula).join(";")}\r\n`;
}

function celula(v: Celula): string {
  if (v === null || v === undefined) return "";
  if (typeof v === "number") return String(v);
  let s = typeof v === "string" ? v : v.texto;
  // Texto vindo do firmware abrindo com `=`, `+`, `-` ou `@` viraria fórmula
  // no Excel. O apóstrofo é o jeito que o próprio Excel usa para "isto é texto".
  if (typeof v !== "string" && /^[=+\-@]/.test(s)) s = `'${s}`;
  return /[;"\r\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
}

function decimal(n: number | null, casas: number): string {
  return n === null ? "" : n.toFixed(casas).replace(".", ",");
}

/** `01/10/2026 22:15:03`, no fuso de exibição. */
function formatadorData(tz: string): (d: Date) => string {
  const f = new Intl.DateTimeFormat("pt-BR", {
    timeZone: tz,
    day: "2-digit",
    month: "2-digit",
    year: "numeric",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hourCycle: "h23",
  });
  return (d) => f.format(d).replace(",", "");
}

function fluxo(gerador: AsyncGenerator<string>): ReadableStream<Uint8Array> {
  const enc = new TextEncoder();
  return new ReadableStream<Uint8Array>({
    async pull(controller) {
      // Junta várias linhas por pedaço: um `enqueue` por linha custaria mais
      // em overhead de fluxo do que a própria linha.
      let pedaco = "";
      while (pedaco.length < 64 * 1024) {
        const { value, done } = await gerador.next();
        if (done) {
          if (pedaco) controller.enqueue(enc.encode(pedaco));
          controller.close();
          return;
        }
        pedaco += value;
      }
      controller.enqueue(enc.encode(pedaco));
    },
    async cancel() {
      await gerador.return(undefined);
    },
  });
}
