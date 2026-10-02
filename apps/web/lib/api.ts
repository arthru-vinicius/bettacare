import type {
  CommandAction,
  CommandItem,
  DeviceConfig,
  EventsResponse,
  OverviewResponse,
  ReportPoint,
} from "@bettacare/contract";

/**
 * Cliente da API do navegador.
 *
 * Sempre caminho relativo: a interface é um export estático servido pela mesma
 * origem, então não existe nenhuma `NEXT_PUBLIC_*` com host para congelar no
 * bundle. É o que faz o mesmo artefato rodar em qualquer host.
 *
 * Não há token nem cabeçalho de autenticação: o Cloudflare Access autentica
 * antes da requisição chegar ao servidor. Requisição não autenticada é barrada
 * na borda e nunca toca a máquina.
 */

export class ApiError extends Error {
  constructor(
    readonly status: number,
    message: string,
    /**
     * O `error` do corpo JSON, quando o servidor manda um (`db_unavailable`,
     * `device_not_found`, …). Sem isto o front só sabia dizer "não foi
     * possível falar com o servidor" para qualquer 5xx — inclusive quando o
     * servidor estava de pé e só o banco é que estava fora, um erro real que
     * já tinha acontecido e apontava o dedo para a peça errada.
     */
    readonly code?: string,
  ) {
    super(message);
    this.name = "ApiError";
  }
}

async function get<T>(path: string, signal?: AbortSignal): Promise<T> {
  const r = await fetch(path, {
    signal: signal ?? null,
    headers: { accept: "application/json" },
    // A API nunca deve vir do cache do navegador: é estado corrente.
    cache: "no-store",
  });
  if (!r.ok) {
    const code = await readErrorCode(r);
    throw new ApiError(r.status, `${path} respondeu ${r.status}`, code);
  }
  return (await r.json()) as T;
}

/** Lê `{"error": "..."}` do corpo, tolerando corpo vazio ou não-JSON. */
async function readErrorCode(r: Response): Promise<string | undefined> {
  try {
    const body = (await r.clone().json()) as { error?: unknown };
    return typeof body.error === "string" ? body.error : undefined;
  } catch {
    return undefined;
  }
}

export function fetchOverview(signal?: AbortSignal): Promise<OverviewResponse> {
  return get<OverviewResponse>("/api/overview", signal);
}

export interface EventQuery {
  sev?: string[] | undefined;
  comp?: string[] | undefined;
  source?: "device" | "server" | undefined;
  /** Busca em `msg` e `code` — o servidor já aceita (`docs/api-servidor.md`); só faltava o front pedir (UPGRADE/06, P2). */
  q?: string | undefined;
  /** Início do período, inclusivo. */
  from?: Date | undefined;
  /** Fim do período, exclusivo. */
  to?: Date | undefined;
  cursor?: string | null | undefined;
  limit?: number | undefined;
}

/** Os filtros como query string — os mesmos para a lista e para a planilha. */
function eventParams(q: EventQuery): URLSearchParams {
  const p = new URLSearchParams();
  if (q.sev?.length) p.set("sev", q.sev.join(","));
  if (q.comp?.length) p.set("comp", q.comp.join(","));
  if (q.source) p.set("source", q.source);
  if (q.q?.trim()) p.set("q", q.q.trim());
  if (q.from) p.set("from", q.from.toISOString());
  if (q.to) p.set("to", q.to.toISOString());
  return p;
}

export function fetchEvents(
  q: EventQuery,
  signal?: AbortSignal,
): Promise<EventsResponse> {
  const p = eventParams(q);
  if (q.cursor) p.set("cursor", q.cursor);
  p.set("limit", String(q.limit ?? 40));
  return get<EventsResponse>(`/api/events?${p}`, signal);
}

/**
 * Link da planilha. O navegador baixa direto, sem passar pelo `fetch` — é o
 * que faz o "Salvar como" funcionar no celular, e os cookies do Cloudflare
 * Access vão junto como em qualquer outra chamada.
 */
export function exportUrl(tipo: "events" | "telemetry", q: EventQuery): string {
  const p = tipo === "events" ? eventParams(q) : new URLSearchParams();
  if (tipo === "telemetry") {
    if (q.from) p.set("from", q.from.toISOString());
    if (q.to) p.set("to", q.to.toISOString());
  }
  return `/api/export/${tipo}.csv?${p}`;
}

/**
 * Grava a configuração inteira — o servidor valida e devolve a versão nova,
 * que o aquário passa a comparar no próximo POST.
 */
export async function saveConfig(config: DeviceConfig): Promise<number> {
  const r = await fetch("/api/settings", {
    method: "PUT",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(config),
  });
  if (!r.ok) {
    const code = await readErrorCode(r);
    throw new ApiError(r.status, `configuração recusada (${r.status})`, code);
  }
  const body = (await r.json()) as { config_version: number };
  return body.config_version;
}

export async function fetchCommands(
  limit = 20,
  signal?: AbortSignal,
): Promise<CommandItem[]> {
  const r = await get<{ items: CommandItem[] }>(
    `/api/commands?limit=${limit}`,
    signal,
  );
  return r.items;
}

export async function fetchReport(
  days: number,
  signal?: AbortSignal,
): Promise<ReportPoint[]> {
  const to = new Date();
  const from = new Date(to.getTime() - days * 24 * 60 * 60 * 1000);
  const r = await get<{ items: ReportPoint[] }>(
    `/api/report?from=${from.toISOString()}&to=${to.toISOString()}`,
    signal,
  );
  return r.items;
}

/**
 * Enfileira um comando.
 *
 * Responde 202: o comando foi aceito, mas só será executado quando o
 * dispositivo buscar a fila no próximo POST — um intervalo de telemetria
 * depois, no máximo. Quem acompanha o desfecho é `fetchCommands`, pelo
 * `status`, não esta chamada.
 */
export async function sendCommand(action: CommandAction): Promise<number> {
  const r = await fetch("/api/commands", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(action),
  });
  if (!r.ok) {
    const code = await readErrorCode(r);
    throw new ApiError(r.status, `comando recusado (${r.status})`, code);
  }
  const body = (await r.json()) as { id: number };
  return body.id;
}
