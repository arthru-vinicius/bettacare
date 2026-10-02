/**
 * Formatação para apresentação.
 *
 * O banco guarda tudo em UTC — exigência do contrato do homelab. O fuso só
 * aparece aqui, e vem do navegador do usuário, que é quem sabe onde ele está.
 */

/** `26,5` — vírgula decimal, como se escreve em português. */
export function formatTemp(c: number | null | undefined): string {
  return c === null || c === undefined ? "—" : c.toFixed(1).replace(".", ",");
}

/** `1.420` — milhar com ponto. */
export function formatInt(n: number | null | undefined): string {
  return n === null || n === undefined ? "—" : n.toLocaleString("pt-BR");
}

/** `22:15:03`. */
export function formatClock(iso: string): string {
  return new Date(iso).toLocaleTimeString("pt-BR", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
}

/** Chave do dia no fuso do navegador, para agrupar a lista de registros. */
export function dayKey(iso: string): string {
  const d = new Date(iso);
  return `${d.getFullYear()}-${d.getMonth()}-${d.getDate()}`;
}

/** "Hoje", "Ontem" ou "terça, 30 de setembro". */
export function formatDayHeader(iso: string): string {
  const d = new Date(iso);
  const hoje = new Date();
  const ontem = new Date(hoje.getFullYear(), hoje.getMonth(), hoje.getDate() - 1);
  if (d.toDateString() === hoje.toDateString()) return "Hoje";
  if (d.toDateString() === ontem.toDateString()) return "Ontem";
  return d.toLocaleDateString("pt-BR", { weekday: "long", day: "numeric", month: "long" });
}

/** Segundos do intervalo de telemetria, para frases como "a cada 1 s". */
export function formatInterval(ms: number | null | undefined): string {
  if (!ms) return "alguns segundos";
  const s = ms / 1000;
  return `${s.toLocaleString("pt-BR")} ${s === 1 ? "segundo" : "segundos"}`;
}

export function formatTime(iso: string | null | undefined): string {
  if (!iso) return "—";
  return new Date(iso).toLocaleTimeString("pt-BR", {
    hour: "2-digit",
    minute: "2-digit",
  });
}

export function formatDateTime(iso: string | null | undefined): string {
  if (!iso) return "—";
  const d = new Date(iso);
  const hoje = new Date();
  const mesmoDia = d.toDateString() === hoje.toDateString();
  return mesmoDia
    ? d.toLocaleTimeString("pt-BR", { hour: "2-digit", minute: "2-digit", second: "2-digit" })
    : d.toLocaleString("pt-BR", {
        day: "2-digit",
        month: "2-digit",
        hour: "2-digit",
        minute: "2-digit",
      });
}

/** "há 3 min", "há 2 h". Mais legível que um horário absoluto para durações curtas. */
export function timeAgo(iso: string | null | undefined): string {
  if (!iso) return "—";
  return secondsAgo(Math.floor((Date.now() - new Date(iso).getTime()) / 1000));
}

/** "há 3 min" a partir de uma idade já em segundos — sem relógio, pura. */
export function secondsAgo(s: number): string {
  if (s < 60) return "agora há pouco";
  const min = Math.floor(s / 60);
  if (min < 60) return `há ${min} min`;
  const h = Math.floor(min / 60);
  if (h < 24) return `há ${h} h`;
  const d = Math.floor(h / 24);
  return `há ${d} ${d === 1 ? "dia" : "dias"}`;
}

export function formatDuration(minutos: number): string {
  if (minutos < 60) return `${minutos} min`;
  const h = Math.floor(minutos / 60);
  const m = minutos % 60;
  return m === 0 ? `${h} h` : `${h} h ${m} min`;
}
