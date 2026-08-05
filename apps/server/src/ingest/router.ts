import {
  INGEST_TOKEN_HEADER,
  TELEMETRY_PATH,
  telemetryRequestSchema,
} from "@bettacare/contract";
import { Hono } from "hono";

import { pingDatabase } from "../db/client.js";
import type { Runtime } from "../runtime.js";
import { tokenMatches } from "./auth.js";
import { processTelemetry } from "./process.js";

/**
 * O roteador da porta do ESP32 (8080), publicada com bind explícito no IP da
 * LAN.
 *
 * É **deliberadamente separado** do roteador da interface. Uma requisição que
 * chegue aqui não alcança nenhuma rota da UI, e o middleware que lê
 * `Cf-Access-Authenticated-User-Email` sequer é registrado nesta porta: aquele
 * tráfego não passa pelo Cloudflare Access, então confiar no cabeçalho aqui
 * seria confiar em qualquer um da rede local.
 */
export function createIngestRouter(rt: Runtime): Hono {
  const app = new Hono();

  app.get("/healthz", async (c) => {
    if (!rt.isDbReady()) {
      return c.json({ status: "starting" }, 503);
    }
    const alive = await pingDatabase(rt.pool);
    return alive
      ? c.json({ status: "ok" })
      : c.json({ status: "degraded" }, 503);
  });

  app.post(TELEMETRY_PATH, async (c) => {
    if (!tokenMatches(c.req.header(INGEST_TOKEN_HEADER), rt.cfg.DEVICE_INGEST_TOKEN)) {
      rt.log.warn(
        { ip: c.req.header("x-forwarded-for") ?? "lan" },
        "token de ingestão recusado",
      );
      return c.json({ ok: false, error: "unauthorized" }, 401);
    }

    // Sem banco o servidor não tem o que prometer. Corpo curto: o dispositivo
    // só precisa saber que falhou, e continua operando com a última
    // configuração conhecida no NVS.
    if (!rt.isDbReady()) {
      return c.json({ ok: false, error: "db_unavailable" }, 503);
    }

    let raw: unknown;
    try {
      raw = await c.req.json();
    } catch {
      return c.json({ ok: false, error: "invalid_json" }, 400);
    }

    const parsed = telemetryRequestSchema.safeParse(raw);
    if (!parsed.success) {
      rt.log.warn(
        { issues: parsed.error.issues.slice(0, 5) },
        "telemetria rejeitada pelo contrato",
      );
      return c.json({ ok: false, error: "invalid_body" }, 400);
    }

    const now = new Date();
    try {
      const result = await processTelemetry(rt.db, rt.cfg, parsed.data, now);

      rt.log.debug(
        {
          device: parsed.data.device_id,
          seq: parsed.data.seq,
          duplicate: result.duplicate,
          history: result.wroteHistory,
          commands: result.response.commands.length,
        },
        "telemetria processada",
      );

      return c.json(result.response);
    } catch (err) {
      rt.log.error({ err }, "falha ao processar telemetria");
      return c.json({ ok: false, error: "internal" }, 500);
    }
  });

  app.all("*", (c) => c.json({ ok: false, error: "not_found" }, 404));

  return app;
}
