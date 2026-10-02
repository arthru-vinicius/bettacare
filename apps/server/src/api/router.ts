import { pushSubscriptionSchema } from "@bettacare/contract";
import { Hono } from "hono";

import { pingDatabase } from "../db/client.js";
import type { Runtime } from "../runtime.js";
import {
  pushConfigured,
  sendToAll,
  subscribe,
  unsubscribe,
} from "../push/notify.js";
import { accessIdentity, currentUser } from "./access.js";
import { eventosCsv, medicoesCsv, nomeDoArquivo, parsePeriodo } from "./export.js";
import {
  enqueueCommand,
  getOverview,
  getReport,
  listCommands,
  listEvents,
  parseCommand,
  updateSettings,
} from "./queries.js";

/** Um aquário. O identificador é parâmetro para não travar o schema num só. */
const DEFAULT_DEVICE = "aquarium-01";

export function createApiRouter(rt: Runtime): Hono {
  const app = new Hono();

  // Sem autenticação e barato, como o contrato exige.
  app.get("/healthz", async (c) => {
    if (!rt.isDbReady()) return c.json({ status: "starting" }, 503);
    const alive = await pingDatabase(rt.pool);
    return alive ? c.json({ status: "ok" }) : c.json({ status: "degraded" }, 503);
  });

  const api = new Hono();
  api.use("*", accessIdentity(rt.cfg));

  api.use("*", async (c, next) => {
    if (!rt.isDbReady()) return c.json({ error: "db_unavailable" }, 503);
    await next();
  });

  api.get("/me", (c) => c.json({ email: currentUser(c) }));

  api.get("/overview", async (c) => {
    const device = c.req.query("device") ?? DEFAULT_DEVICE;
    const data = await getOverview(rt.db, device);
    return data === null ? c.json({ error: "device_not_found" }, 404) : c.json(data);
  });

  api.get("/events", async (c) => {
    const q = c.req.query();
    const limit = Math.min(Number(q["limit"] ?? 50) || 50, 200);
    const from = dataOpcional(q["from"]);
    const to = dataOpcional(q["to"]);
    if (from === null || to === null) return c.json({ error: "invalid_range" }, 400);

    const page = await listEvents(rt.db, {
      ...filtroDeEventos(q),
      from,
      to,
      limit,
      cursor: q["cursor"] || undefined,
    });

    return c.json(page);
  });

  // ── Planilhas ───────────────────────────────────────────────────────────
  //
  // O navegador baixa direto pelo link — os cookies do Cloudflare Access vão
  // junto como em qualquer outra chamada da interface.
  api.get("/export/events.csv", (c) => {
    const q = c.req.query();
    const periodo = parsePeriodo(q["from"], q["to"]);
    if (periodo === null) return c.json({ error: "invalid_range" }, 400);

    const tz = rt.cfg.TZ_DISPLAY;
    return new Response(eventosCsv(rt.db, { ...filtroDeEventos(q), ...periodo }, tz), {
      headers: cabecalhosCsv(nomeDoArquivo("registros", periodo, tz)),
    });
  });

  api.get("/export/telemetry.csv", (c) => {
    const q = c.req.query();
    const periodo = parsePeriodo(q["from"], q["to"]);
    if (periodo === null) return c.json({ error: "invalid_range" }, 400);

    const tz = rt.cfg.TZ_DISPLAY;
    const deviceId = q["device"] ?? DEFAULT_DEVICE;
    return new Response(medicoesCsv(rt.db, { deviceId, ...periodo }, tz), {
      headers: cabecalhosCsv(nomeDoArquivo("medicoes", periodo, tz)),
    });
  });

  api.get("/commands", async (c) => {
    const device = c.req.query("device") ?? DEFAULT_DEVICE;
    const limit = Math.min(Number(c.req.query("limit") ?? 30) || 30, 100);
    return c.json({ items: await listCommands(rt.db, device, limit) });
  });

  api.post("/commands", async (c) => {
    const device = c.req.query("device") ?? DEFAULT_DEVICE;

    let raw: unknown;
    try {
      raw = await c.req.json();
    } catch {
      return c.json({ error: "invalid_json" }, 400);
    }

    const parsed = parseCommand(raw);
    if (!parsed.success) {
      return c.json({ error: "invalid_command", issues: parsed.error.issues }, 400);
    }

    const email = currentUser(c);
    const id = await enqueueCommand(rt.db, device, parsed.data, email);

    rt.log.info(
      { device, action: parsed.data.action, by: email, id },
      "comando enfileirado",
    );

    // 202, não 200: o comando foi aceito, mas só será executado quando o
    // dispositivo buscar a fila no próximo POST. A interface acompanha o
    // desfecho pelo status do comando, não por esta resposta.
    return c.json({ id, status: "queued" }, 202);
  });

  api.get("/settings", async (c) => {
    const device = c.req.query("device") ?? DEFAULT_DEVICE;
    const data = await getOverview(rt.db, device);
    return data === null
      ? c.json({ error: "device_not_found" }, 404)
      : c.json({ config: data.config, config_version: data.config_version });
  });

  api.put("/settings", async (c) => {
    const device = c.req.query("device") ?? DEFAULT_DEVICE;

    let raw: unknown;
    try {
      raw = await c.req.json();
    } catch {
      return c.json({ error: "invalid_json" }, 400);
    }

    const email = currentUser(c);
    const result = await updateSettings(rt.db, device, raw, email);
    if (!result.ok) {
      return c.json({ error: "invalid_config", issues: result.issues }, 400);
    }

    if (result.changed) {
      rt.log.info({ device, by: email, version: result.version }, "configuração alterada");
    }
    return c.json({ config_version: result.version });
  });

  // ── Notificações push ───────────────────────────────────────────────────
  //
  // A chave pública é o que o navegador precisa para se inscrever. Não é
  // segredo — ela existe justamente para ser distribuída.
  api.get("/push/key", (c) =>
    c.json({ key: rt.vapidPublicKey(), enabled: pushConfigured() }),
  );

  api.post("/push/subscribe", async (c) => {
    let raw: unknown;
    try {
      raw = await c.req.json();
    } catch {
      return c.json({ error: "invalid_json" }, 400);
    }

    const parsed = pushSubscriptionSchema.safeParse(raw);
    if (!parsed.success) {
      return c.json({ error: "invalid_subscription", issues: parsed.error.issues }, 400);
    }

    await subscribe(rt.db, parsed.data, currentUser(c));
    rt.log.info({ by: currentUser(c) }, "inscrição push registrada");
    return c.json({ ok: true }, 201);
  });

  api.delete("/push/subscribe", async (c) => {
    let raw: unknown;
    try {
      raw = await c.req.json();
    } catch {
      return c.json({ error: "invalid_json" }, 400);
    }
    const endpoint = (raw as { endpoint?: unknown }).endpoint;
    if (typeof endpoint !== "string") {
      return c.json({ error: "invalid_subscription" }, 400);
    }

    await unsubscribe(rt.db, endpoint);
    return c.json({ ok: true });
  });

  /** Envia uma notificação de teste — é como o usuário confirma que funcionou. */
  api.post("/push/test", async (c) => {
    await sendToAll(rt, {
      title: "BettaCare",
      body: "Notificações ativadas. É assim que um alerta vai chegar.",
      tag: "teste",
    });
    return c.json({ ok: true });
  });

  api.get("/report", async (c) => {
    const device = c.req.query("device") ?? DEFAULT_DEVICE;
    const to = c.req.query("to") ? new Date(c.req.query("to")!) : new Date();
    const from = c.req.query("from")
      ? new Date(c.req.query("from")!)
      : new Date(to.getTime() - 7 * 24 * 60 * 60 * 1000);

    if (Number.isNaN(from.getTime()) || Number.isNaN(to.getTime())) {
      return c.json({ error: "invalid_range" }, 400);
    }

    return c.json({ items: await getReport(rt.db, device, from, to) });
  });

  app.route("/api", api);
  return app;
}

/** Os filtros de evento que vêm da query — iguais na lista e na planilha. */
function filtroDeEventos(q: Record<string, string>) {
  return {
    deviceId: q["device"] ?? DEFAULT_DEVICE,
    comps: q["comp"]?.split(",").filter(Boolean),
    sevs: q["sev"]?.split(",").filter(Boolean),
    source:
      q["source"] === "device" || q["source"] === "server"
        ? (q["source"] as "device" | "server")
        : undefined,
    search: q["q"] || undefined,
  };
}

/** Ausente → `undefined` (sem filtro); presente e inválida → `null` (400). */
function dataOpcional(raw: string | undefined): Date | undefined | null {
  if (!raw) return undefined;
  const d = new Date(raw);
  return Number.isNaN(d.getTime()) ? null : d;
}

function cabecalhosCsv(nome: string): Record<string, string> {
  return {
    "content-type": "text/csv; charset=utf-8",
    "content-disposition": `attachment; filename="${nome}"`,
    // Planilha é retrato de um instante: nunca do cache.
    "cache-control": "no-store",
  };
}
