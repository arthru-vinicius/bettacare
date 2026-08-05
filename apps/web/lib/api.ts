import type {
  CommandAction,
  CommandItem,
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
  if (!r.ok) throw new ApiError(r.status, `${path} respondeu ${r.status}`);
  return (await r.json()) as T;
}

export function fetchOverview(signal?: AbortSignal): Promise<OverviewResponse> {
  return get<OverviewResponse>("/api/overview", signal);
}

export interface EventQuery {
  sev?: string[] | undefined;
  comp?: string[] | undefined;
  source?: "device" | "server" | undefined;
  cursor?: string | null | undefined;
  limit?: number | undefined;
}

export function fetchEvents(
  q: EventQuery,
  signal?: AbortSignal,
): Promise<EventsResponse> {
  const p = new URLSearchParams();
  if (q.sev?.length) p.set("sev", q.sev.join(","));
  if (q.comp?.length) p.set("comp", q.comp.join(","));
  if (q.source) p.set("source", q.source);
  if (q.cursor) p.set("cursor", q.cursor);
  p.set("limit", String(q.limit ?? 40));
  return get<EventsResponse>(`/api/events?${p}`, signal);
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
 * dispositivo buscar a fila no próximo POST — até 3 segundos depois. Quem
 * acompanha o desfecho é `fetchCommands`, pelo `status`, não esta chamada.
 */
export async function sendCommand(action: CommandAction): Promise<number> {
  const r = await fetch("/api/commands", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(action),
  });
  if (!r.ok) throw new ApiError(r.status, `comando recusado (${r.status})`);
  const body = (await r.json()) as { id: number };
  return body.id;
}
