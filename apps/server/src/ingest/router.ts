import {
  INGEST_TOKEN_HEADER,
  sanitizeTelemetryRequest,
  TELEMETRY_PATH,
} from "@bettacare/contract";
import { Hono } from "hono";

import { pingDatabase } from "../db/client.js";
import { sendToAll } from "../push/notify.js";
import type { Runtime } from "../runtime.js";
import { tokenMatches } from "./auth.js";
import { processTelemetry } from "./process.js";

/**
 * Teto do corpo do POST, verificado **antes** de ler.
 *
 * O firmware mede o corpo real entre 391 e 777 bytes (`docs/firmware.md`). O
 * modelo de ameaça aqui não é o invasor — é o **firmware com defeito**: um
 * `_build_body` em laço, um buffer de eventos corrompido, e o servidor
 * compartilhado do homelab recebe um corpo de tamanho arbitrário. 16 KB dá
 * vinte vezes de folga sobre o pior caso legítimo. Ver UPGRADE/04 (S11).
 */
const MAX_BODY_BYTES = 16 * 1024;

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

    const declaredLength = Number(c.req.header("content-length") ?? "0");
    if (declaredLength > MAX_BODY_BYTES) {
      rt.log.warn({ declaredLength }, "corpo do ingest recusado por tamanho");
      return c.json({ ok: false, error: "payload_too_large" }, 413);
    }

    let raw: unknown;
    try {
      raw = await c.req.json();
    } catch {
      return c.json({ ok: false, error: "invalid_json" }, 400);
    }

    /**
     * Tolerância a dado ruim (UPGRADE/05, achado C1): um único valor fora de
     * faixa — o exemplo real é `fan.rpm` inflado por ruído no tacômetro sem
     * pull-up — não pode derrubar o corpo inteiro. Rejeitar tudo faz o
     * firmware reenviar o mesmo corpo indefinidamente e o watchdog concluir
     * que o dispositivo sumiu, quando ele só tentou contar o que aconteceu.
     */
    const sanitized = sanitizeTelemetryRequest(raw);
    if (sanitized.data === null) {
      // O valor recebido vai junto de cada problema: sem ele, "comp fora do
      // enum" não dizia qual componente o firmware mandou (UPGRADE/07).
      rt.log.warn(
        { problems: sanitized.problems, bodyPreview: previewOf(raw) },
        "telemetria rejeitada pelo contrato",
      );
      return c.json({ ok: false, error: "invalid_body" }, 400);
    }

    const now = new Date();
    try {
      const result = await processTelemetry(
        rt.db,
        rt.cfg,
        sanitized.data,
        now,
        sanitized.corrections,
      );

      // `warn` só para a correção que abriu evento; a repetida, que só somou
      // `repeat_count`, vai para `debug` — um campo errado em todo POST era
      // um `warn` por segundo no log do homelab.
      const repetidas = sanitized.corrections.filter((c) => !result.newCorrections.includes(c));
      if (result.newCorrections.length > 0) {
        rt.log.warn(
          { device: sanitized.data.device_id, corrections: result.newCorrections },
          "telemetria aceita com correções — ver eventos ingest.field_rejected/ingest.event_dropped",
        );
      }
      if (repetidas.length > 0) {
        rt.log.debug(
          { device: sanitized.data.device_id, corrections: repetidas },
          "correções repetidas, somadas ao evento já aberto",
        );
      }

      rt.log.debug(
        {
          device: sanitized.data.device_id,
          seq: sanitized.data.seq,
          duplicate: result.duplicate,
          history: result.wroteHistory,
          commands: result.response.commands.length,
        },
        "telemetria processada",
      );

      /**
       * Notifica **depois** de responder ao dispositivo, sem `await`.
       *
       * O ESP32 tem timeout de 4 s; fazê-lo esperar uma entrega a um push
       * service externo transformaria um alerta em causa de falha de
       * telemetria. O `catch` é obrigatório aqui: sem ele, uma rejeição vira
       * `unhandledRejection`.
       */
      for (const alerta of result.alerts) {
        void sendToAll(rt, alerta).catch((err) =>
          rt.log.error({ err }, "falha ao notificar alerta de saúde"),
        );
      }

      return c.json(result.response);
    } catch (err) {
      rt.log.error({ err }, "falha ao processar telemetria");
      return c.json({ ok: false, error: "internal" }, 500);
    }
  });

  app.all("*", (c) => c.json({ ok: false, error: "not_found" }, 404));

  return app;
}

/** Corpo truncado para log — nunca o corpo inteiro, que pode ser garbage grande. */
function previewOf(raw: unknown): string {
  try {
    return JSON.stringify(raw).slice(0, 300);
  } catch {
    return "<não serializável>";
  }
}
