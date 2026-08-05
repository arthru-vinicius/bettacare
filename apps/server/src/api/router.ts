import { Hono } from "hono";

import { pingDatabase } from "../db/client.js";
import type { Runtime } from "../runtime.js";
import { accessIdentity, currentUser } from "./access.js";
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

    const page = await listEvents(rt.db, {
      deviceId: q["device"] ?? DEFAULT_DEVICE,
      comps: q["comp"]?.split(",").filter(Boolean),
      sevs: q["sev"]?.split(",").filter(Boolean),
      source: q["source"] === "device" || q["source"] === "server" ? q["source"] : undefined,
      search: q["q"] || undefined,
      limit,
      cursor: q["cursor"] || undefined,
    });

    return c.json(page);
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

    rt.log.info({ device, by: email, version: result.version }, "configuração alterada");
    return c.json({ config_version: result.version });
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
