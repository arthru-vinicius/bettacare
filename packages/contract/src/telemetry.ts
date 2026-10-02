import { z } from "zod";
import { commandAckSchema, pendingCommandSchema } from "./commands.js";
import { deviceConfigSchema } from "./config.js";
import { deviceEventSchema, MAX_EVENTS_PER_POST } from "./events.js";
import { deviceHealthReportSchema } from "./health.js";
import {
  componentSchema,
  deviceIdSchema,
  healthStatusSchema,
  INT32_MAX,
  isoInstantSchema,
  timeOfDaySchema,
  UINT32_MAX,
} from "./primitives.js";

/**
 * `POST /api/v1/telemetry` — o único endpoint que o ESP32 usa.
 *
 * Um request serve os dois sentidos: o corpo leva o estado do aquário, a
 * resposta traz os comandos pendentes. O servidor **nunca inicia conexão**, e é
 * por isso que o IP dinâmico do dispositivo deixa de ser um problema.
 */

export const TELEMETRY_PATH = "/api/v1/telemetry";
export const INGEST_TOKEN_HEADER = "x-api-token";
export const ACCESS_EMAIL_HEADER = "cf-access-authenticated-user-email";

// ── Blocos de estado ──────────────────────────────────────────────────────

export const lightStateSchema = z.object({
  on: z.boolean(),
  /**
   * Quem determinou o estado atual. Distingue "eu apaguei" de "a automação
   * apagou" — e sem isso o histórico de luz fica indecifrável.
   */
  source: z.enum(["schedule", "manual", "button", "command", "boot"]),
});
export type LightState = z.infer<typeof lightStateSchema>;

/**
 * Faixas da água **nesta instalação** — aquário em Recife, com ar-condicionado.
 *
 * Fora de `TEMP_PLAUSIBLE_C` a leitura é defeito, nunca temperatura: em
 * produção apareceram dois -48,00 °C isolados entre leituras de 27,13 °C, um
 * quadro de 1-Wire que passou no CRC por acaso. Não entra em lugar nenhum —
 * nem no banco, nem na saúde, nem no gráfico. O firmware (2.0.2) descarta na
 * origem; o servidor confere de novo.
 *
 * Fora de `TEMP_USUAL_C` a leitura é real, mas rara: sai do gráfico (que
 * mostra o comportamento normal da água) e vira um aviso nos Registros, com
 * quanto tempo ficou lá e o pico (`temp.out_of_usual_range`).
 */
export const TEMP_PLAUSIBLE_C = { min: 10, max: 45 } as const;
export const TEMP_USUAL_C = { min: 16, max: 33 } as const;

export const temperatureStateSchema = z.object({
  /** Nulo quando o sensor não respondeu. Não usar 0 nem -127 como sentinela. */
  celsius: z.number().min(-55).max(125).nullable(),
  /** O sensor foi encontrado no barramento 1-Wire? */
  available: z.boolean(),
  /** A última leitura passou no CRC? */
  valid: z.boolean(),
  /**
   * Idade da leitura em ms — é o que define `temp.stale`.
   *
   * **Nulo quando nunca houve leitura válida.** A idade de algo que não
   * aconteceu não é um número grande, é ausência — e tratá-la como número foi
   * o bug que derrubava o ingest: o firmware mandava `UINT32_MAX` e a coluna
   * `integer` estourava.
   */
  age_ms: z.int().min(0).max(UINT32_MAX).nullable(),
});
export type TemperatureState = z.infer<typeof temperatureStateSchema>;

export const fanStateSchema = z.object({
  on: z.boolean(),
  speed_percent: z.int().min(0).max(100),
  /**
   * Rotação medida pelo tacômetro. É a **única realimentação real** do sistema:
   * `speed_percent > 0` com `rpm == 0` significa que a ventoinha não gira.
   */
  rpm: z.int().min(0).max(20000),
  mode: z.enum(["auto", "manual", "manual_off", "failsafe"]),
});
export type FanState = z.infer<typeof fanStateSchema>;

export const rtcStateSchema = z.object({
  available: z.boolean(),
  time: timeOfDaySchema.nullable(),
  /** O DS3231 acusou perda de energia — bateria no fim. */
  lost_power: z.boolean().default(false),
});
export type RtcState = z.infer<typeof rtcStateSchema>;

export const wifiStateSchema = z.object({
  /**
   * Nulo quando não havia associação no instante da leitura (UPGRADE/03,
   * F13). Antes o firmware usava `-120` como sentinela de "desconectado", e
   * um snapshot capturado durante uma queda breve podia viajar como se fosse
   * medição real — a mesma família do bug de sentinela numérica corrigido no
   * commit `a4596fe`.
   */
  rssi: z.int().min(-120).max(0).nullable(),
  ip: z.ipv4().nullable(),
  /** Reconexões desde o boot. Subindo depressa = link instável. */
  reconnects: z.int().min(0).max(INT32_MAX).default(0),
});
export type WifiState = z.infer<typeof wifiStateSchema>;

/**
 * Estado do módulo de alimentação de precisão — opcional por natureza: pode
 * nunca ter sido instalado, e mesmo instalado não fica ligado o tempo todo
 * (é desenho, não defeito — ver `docs/pinagem-alimentador-modulo.md`).
 *
 * `connected` é o único campo sempre presente quando o bloco existe. Os
 * demais só viajam quando o módulo respondeu neste ciclo; ausentes, o
 * servidor preserva o último valor conhecido (mesmo padrão de
 * `lightDesired` em `ingest/process.ts`) — desconectar não apaga a agenda
 * que o app mostra, só marca que não há contato agora.
 */
export const feederStateSchema = z.object({
  connected: z.boolean(),
  auto_enabled: z.boolean().optional(),
  hour1: z.int().min(0).max(23).optional(),
  hour2: z.int().min(0).max(23).optional(),
  grains_per_feeding: z.int().min(1).max(20).optional(),
  /**
   * Segundos desde a última alimentação, medidos pelo ESP32 no instante do
   * POST — não um timestamp do módulo, que tem seu próprio RTC e nenhuma
   * garantia de estar sincronizado com o do servidor. Nulo se o módulo nunca
   * alimentou desde que foi conhecido.
   */
  last_feed_age_s: z.int().min(0).max(INT32_MAX).nullable().optional(),
  last_feed_requested: z.int().min(0).max(20).optional(),
  last_feed_confirmed: z.int().min(0).max(20).optional(),
  last_feed_ok: z.boolean().optional(),
  /**
   * Refeições nas últimas 24 h, contadas pelo módulo — agenda, botão e app
   * juntos. É o que o app compara com `FEEDER_MEALS_PER_24H` antes de
   * oferecer "alimentar mesmo assim".
   */
  meals_24h: z.int().min(0).max(255).optional(),
});
export type FeederState = z.infer<typeof feederStateSchema>;

/**
 * Refeições permitidas em 24 h, somando agenda, botão e app. Além disso, o
 * módulo recusa (`feeder.limit_reached`); só o app pode passar por cima,
 * com `force` no `feeder.feed_now`. O botão do módulo nunca passa.
 */
export const FEEDER_MEALS_PER_24H = 3;

/**
 * Diagnóstico do **controlador**, não do aquário — a distinção que faltava
 * para responder "o ESP32 reiniciou esta noite, e por quê?" ou "a memória
 * livre está caindo ao longo dos dias?". Ver UPGRADE/03 (F7) e UPGRADE/04 (S1).
 *
 * Bloco opcional de propósito: um firmware antigo, que ainda não o envia,
 * continua sendo aceito durante a transição.
 */
export const diagSchema = z.object({
  /** Motivo do último reset (`esp_reset_reason()`). Distingue brownout de watchdog de reboot comandado. */
  reset_reason: z.string().min(1).max(32).nullable().optional(),
  free_heap: z.int().min(0).max(UINT32_MAX).nullable().optional(),
  /** Pior momento desde o boot — a amostragem a cada 3 s quase nunca pega o pico real. */
  min_free_heap: z.int().min(0).max(UINT32_MAX).nullable().optional(),
  /** Maior bloco alocável. Junto com `free_heap`, é o que denuncia fragmentação. */
  max_alloc_heap: z.int().min(0).max(UINT32_MAX).nullable().optional(),
  post_latency_ms: z.int().min(0).max(60_000).nullable().optional(),
  /** Já existe no firmware (`api_client_consecutive_failures()`); só faltava sair no corpo. */
  api_failures: z.int().min(0).max(65535).default(0),
  pot_raw_adc: z.int().min(0).max(4095).nullable().optional(),
  button_pressed: z.boolean().default(false),
  tach_pulses_raw: z.int().min(0).max(UINT32_MAX).nullable().optional(),
  net_task_stack_hwm: z.int().min(0).max(UINT32_MAX).nullable().optional(),
  boot_count: z.int().min(0).max(INT32_MAX).nullable().optional(),

  /**
   * Falhas de NVS acumuladas desde o boot. `nvs` emitia quatro eventos e não
   * tinha saúde nenhuma — e `nvs.write_failed` significa "a configuração não
   * sobrevive ao próximo reboot", que é exatamente o tipo de coisa que precisa
   * aparecer na aba Saúde em vez de passar no log.
   */
  nvs_failures: z.int().min(0).max(65535).default(0),
  /**
   * Desfecho da última atualização OTA, **persistido em NVS** — o sucesso
   * reinicia o dispositivo, então sem persistir o resultado se perderia
   * justamente no caso bom. `none` = nunca houve OTA neste dispositivo.
   */
  ota_last_result: z.enum(["none", "ok", "failed"]).default("none"),
  /** Eventos descartados por estouro de buffer desde o boot — perda de diagnóstico. */
  events_dropped: z.int().min(0).max(65535).default(0),
});
export type Diag = z.infer<typeof diagSchema>;

// ── Autodiagnóstico sob demanda ─────────────────────────────────────────────

/**
 * Um item do relatório de autodiagnóstico.
 *
 * `detail` é texto pronto para exibição, em português: o firmware é quem sabe
 * o que mediu ("3 dispositivos no barramento 1-Wire", "I²C não respondeu no
 * 0x68"), e reescrever isso do lado do servidor seria adivinhar.
 */
export const diagnosticCheckSchema = z.object({
  comp: componentSchema,
  status: healthStatusSchema,
  detail: z.string().min(1).max(160),
  /** Verdadeiro quando o teste foi de fato executado; falso quando é só leitura passiva. */
  probed: z.boolean().default(false),
});
export type DiagnosticCheck = z.infer<typeof diagnosticCheckSchema>;

/**
 * Relatório devolvido depois de um comando `device.diagnose`.
 *
 * Diferente de `component_status`, que é a saúde **passiva** derivada da
 * telemetria que foi chegando: aqui o firmware **sonda ativamente** cada
 * periférico — varre o barramento 1-Wire, procura o DS3231 no I²C, testa
 * escrita e leitura na NVS, lê o ADC. É a diferença entre "o último dado que
 * recebi parecia bom" e "acabei de conferir".
 */
export const diagnosticReportSchema = z.object({
  /** O comando que pediu este relatório. Nulo se rodou no boot, sem pedido. */
  command_id: z.int().positive().nullable().optional(),
  /** `millis()` no momento da execução — liga o relatório à sessão de boot. */
  ran_at_uptime_ms: z.int().min(0).max(UINT32_MAX),
  duration_ms: z.int().min(0).max(60_000),
  checks: z.array(diagnosticCheckSchema).max(16),
});
export type DiagnosticReport = z.infer<typeof diagnosticReportSchema>;

// ── Requisição ────────────────────────────────────────────────────────────

/**
 * Com `connected: false` não há dado fresco do módulo: o resto do bloco é
 * descartado antes de validar — o que `feederStateSchema` sempre prometeu,
 * agora garantido do lado de cá.
 *
 * O firmware até a 2.0.0 mandava o bloco inteiro em todo POST, zerado quando o
 * módulo nunca tinha respondido desde o boot. `grains_per_feeding: 0` fica
 * abaixo do mínimo e virava correção — um evento `ingest.field_rejected` **por
 * POST** em produção —, e `hour1`/`hour2` zerados passavam na validação e
 * sobrescreviam a agenda: o app mostrava um alimentador que nunca existiu.
 *
 * Fica no corpo da requisição, não em `feederStateSchema`: a mesma forma
 * descreve o `/overview`, onde `connected: false` vem **com** a última agenda
 * conhecida, de propósito.
 */
function dropStaleFeederFields(v: unknown): unknown {
  if (typeof v !== "object" || v === null || Array.isArray(v)) return v;
  return (v as { connected?: unknown }).connected === false ? { connected: false } : v;
}

export const telemetryRequestSchema = z.object({
  device_id: deviceIdSchema,
  /** Contador em NVS, incrementado a cada boot. Detecta reinícios. */
  boot_id: z.int().min(0).max(INT32_MAX),
  /** Monotônico dentro de um boot. É o que dá idempotência ao ingest. */
  seq: z.int().min(0).max(INT32_MAX),
  /** Vem de `millis()`: passa de `INT32_MAX` com ~25 dias de uptime. */
  uptime_ms: z.int().min(0).max(UINT32_MAX),
  /** O que o RTC do dispositivo acha que é. Nulo se o módulo sumiu. */
  device_time: isoInstantSchema.nullable().optional(),
  fw_version: z.string().max(32).optional(),
  config_version: z.int().min(0),

  light: lightStateSchema,
  temperature: temperatureStateSchema,
  fan: fanStateSchema,
  rtc: rtcStateSchema,
  wifi: wifiStateSchema,
  /** Ausente = firmware anterior à integração do alimentador, não "sem módulo". */
  feeder: z.preprocess(dropStaleFeederFields, feederStateSchema).optional(),

  /** A opinião do firmware sobre a própria saúde. Opcional. */
  health: deviceHealthReportSchema.optional(),
  /** Confirmações dos comandos entregues no POST anterior. */
  ack: z.array(commandAckSchema).max(16).default([]),
  /** Eventos acumulados desde o último POST. */
  events: z.array(deviceEventSchema).max(MAX_EVENTS_PER_POST).default([]),
  /** Diagnóstico do controlador. Ver `diagSchema`. */
  diag: diagSchema.optional(),
  /** Relatório de autodiagnóstico, presente só no POST seguinte a um `device.diagnose`. */
  diagnostic: diagnosticReportSchema.optional(),
});
export type TelemetryRequest = z.infer<typeof telemetryRequestSchema>;

// ── Tolerância a dado ruim (UPGRADE/05, achado C1) ──────────────────────────

export interface TelemetryCorrection {
  /** Caminho pontilhado do campo corrigido, ex.: `"fan.rpm"`, ou do item descartado, ex.: `"events.2"`. */
  path: string;
  message: string;
  /**
   * O que foi feito: valor grampeado ao limite, trocado por `null`, ou o item
   * inteiro de uma lista (`events`, `diagnostic.checks`) descartado.
   */
  action: "clamped" | "nulled" | "dropped";
  /** O valor recebido, para o log — sem ele não dá para saber o que o firmware mandou. */
  received: unknown;
}

/** Um problema que não teve conserto, com o valor que chegou. Só para o log. */
export interface TelemetryProblem {
  path: string;
  message: string;
  received: unknown;
}

export interface SanitizedTelemetry {
  /** Nulo só quando a correção pontual não bastou — problema estrutural, não um valor ruim isolado. */
  data: TelemetryRequest | null;
  corrections: TelemetryCorrection[];
  /** Preenchido quando `data` é nulo: o que impediu o aceite. */
  problems: TelemetryProblem[];
}

/**
 * Núcleo do corpo: sem isto não há o que processar, e corrigir seria fingir
 * que o servidor sabe de quem é o POST.
 */
const CORE_PATHS = new Set(["device_id", "boot_id", "seq"]);

/**
 * Campos onde um formato inválido tem um `null` honesto como substituto —
 * o valor deixa de ser "impossível" e passa a ser "não informado".
 */
const NULLABLE_FALLBACK_PATHS = new Set(["wifi.ip", "rtc.time", "device_time"]);

/**
 * Um valor fora de faixa não pode derrubar o corpo inteiro — é exatamente o
 * caminho de falha descrito em UPGRADE/05 (C1): ruído no tacômetro produz
 * `rpm` acima do teto, o corpo inteiro é rejeitado com 400, o firmware reenvia
 * o mesmo corpo para sempre, e o watchdog do servidor conclui que o
 * dispositivo sumiu — quando ele só tentou contar o que estava acontecendo.
 *
 * Tenta o parse estrito primeiro (o caminho normal, sem custo extra). Se
 * falhar, usa os próprios `issues` do Zod para saber exatamente qual campo e
 * por quê: limites numéricos violados são grampeados ao extremo mais próximo,
 * e um punhado de campos conhecidos cai para `null` em vez de bloquear tudo.
 * Corrige e tenta de novo — só devolve `null` se, mesmo corrigido, o corpo
 * continuar inválido, o que indica um problema estrutural e não um valor ruim
 * isolado.
 */
export function sanitizeTelemetryRequest(raw: unknown): SanitizedTelemetry {
  const first = telemetryRequestSchema.safeParse(raw);
  if (first.success) return { data: first.data, corrections: [], problems: [] };

  if (typeof raw !== "object" || raw === null || Array.isArray(raw)) {
    return {
      data: null,
      corrections: [],
      problems: [{ path: "", message: "corpo não é um objeto JSON", received: preview(raw) }],
    };
  }

  // `raw` já é uma árvore JSON (veio de `c.req.json()`), então ida e volta por
  // JSON clona sem risco de perder nada — sem depender de `structuredClone`,
  // que exige `lib: "dom"` para o TypeScript reconhecer.
  const patched = JSON.parse(JSON.stringify(raw)) as Record<string, unknown>;
  const corrections: TelemetryCorrection[] = [];
  const problems: TelemetryProblem[] = [];
  /** Itens a descartar, por lista: `"events"` → índices. */
  const descartes = new Map<string, { path: PropertyKey[]; indices: Set<number> }>();

  for (const issue of first.error.issues) {
    if (issue.path.length === 0) continue;
    const dotted = issue.path.join(".");
    const received = preview(valueAt(raw, issue.path));
    if (CORE_PATHS.has(dotted)) {
      problems.push({ path: dotted, message: issue.message, received });
      continue;
    }

    // Um item ruim de uma lista sai sozinho, sem levar o corpo junto.
    const item = droppableItem(issue.path);
    if (item !== null) {
      const chave = item.listPath.join(".");
      const lista = descartes.get(chave) ?? { path: item.listPath, indices: new Set<number>() };
      if (!lista.indices.has(item.index)) {
        lista.indices.add(item.index);
        corrections.push({
          path: `${chave}.${item.index}`,
          message: issue.message,
          action: "dropped",
          received: preview(valueAt(raw, [...item.listPath, item.index])),
        });
      }
      descartes.set(chave, lista);
      continue;
    }

    const action = applyCorrection(patched, issue, dotted);
    if (action !== null) {
      corrections.push({ path: dotted, message: issue.message, action, received });
    } else {
      problems.push({ path: dotted, message: issue.message, received });
    }
  }

  for (const { path, indices } of descartes.values()) {
    const lista = valueAt(patched, path);
    if (!Array.isArray(lista)) continue;
    // Do maior índice para o menor, para os anteriores não mudarem de posição.
    for (const i of [...indices].sort((a, b) => b - a)) lista.splice(i, 1);
  }

  if (problems.length > 0 || corrections.length === 0) {
    return { data: null, corrections, problems };
  }

  const second = telemetryRequestSchema.safeParse(patched);
  return second.success
    ? { data: second.data, corrections, problems: [] }
    : {
        data: null,
        corrections,
        problems: second.error.issues.map((i) => ({
          path: i.path.join("."),
          message: i.message,
          received: preview(valueAt(patched, i.path)),
        })),
      };
}

/**
 * Listas cujo item inválido é descartado sozinho (UPGRADE/07). Um evento com
 * componente que esta versão do servidor não conhece — o firmware é mais novo
 * — derrubava o POST inteiro, e com ele o estado do aquário: em produção foram
 * 136 corpos perdidos por um `comp: "feeder"` antes da integração do
 * alimentador chegar ao servidor.
 */
function droppableItem(path: PropertyKey[]): { listPath: PropertyKey[]; index: number } | null {
  if (path[0] === "events" && typeof path[1] === "number") {
    return { listPath: ["events"], index: path[1] };
  }
  if (path[0] === "diagnostic" && path[1] === "checks" && typeof path[2] === "number") {
    return { listPath: ["diagnostic", "checks"], index: path[2] };
  }
  return null;
}

function applyCorrection(
  root: Record<string, unknown>,
  issue: { code: string; path: PropertyKey[]; maximum?: unknown; minimum?: unknown },
  dotted: string,
): "clamped" | "nulled" | null {
  const key = issue.path.at(-1);
  if (key === undefined) return null;
  const parent = navigate(root, issue.path.slice(0, -1));
  if (parent === undefined) return null;

  if (issue.code === "too_big" && typeof issue.maximum === "number") {
    parent[key as string] = issue.maximum;
    return "clamped";
  }
  if (issue.code === "too_small" && typeof issue.minimum === "number") {
    parent[key as string] = issue.minimum;
    return "clamped";
  }
  if (NULLABLE_FALLBACK_PATHS.has(dotted)) {
    parent[key as string] = null;
    return "nulled";
  }
  return null;
}

function navigate(
  obj: Record<string, unknown>,
  path: PropertyKey[],
): Record<string, unknown> | undefined {
  const cur = valueAt(obj, path);
  return typeof cur === "object" && cur !== null
    ? (cur as Record<string, unknown>)
    : undefined;
}

function valueAt(obj: unknown, path: PropertyKey[]): unknown {
  let cur: unknown = obj;
  for (const key of path) {
    if (typeof cur !== "object" || cur === null) return undefined;
    cur = (cur as Record<string, unknown>)[key as string];
  }
  return cur;
}

/** O valor recebido, curto o bastante para caber numa linha de log. */
function preview(v: unknown): unknown {
  if (typeof v === "string") return v.length > 80 ? `${v.slice(0, 80)}…` : v;
  if (typeof v === "object" && v !== null) {
    try {
      const s = JSON.stringify(v);
      return s.length > 200 ? `${s.slice(0, 200)}…` : s;
    } catch {
      return "<não serializável>";
    }
  }
  return v;
}

// ── Resposta ──────────────────────────────────────────────────────────────

/**
 * Corpo pequeno e de tamanho previsível — o contrato do homelab exige, e o
 * ESP32 agradece. No caso normal fica em torno de 100 bytes, porque o bloco
 * `config` só viaja quando o `config_version` do dispositivo está atrasado.
 */
export const telemetryResponseSchema = z.object({
  ok: z.literal(true),
  server_time: isoInstantSchema,
  config_version: z.int().min(0),
  config: deviceConfigSchema.optional(),
  commands: z.array(pendingCommandSchema).default([]),
});
export type TelemetryResponse = z.infer<typeof telemetryResponseSchema>;

/** Resposta de erro. Curta de propósito: o ESP32 só precisa saber que falhou. */
export const telemetryErrorSchema = z.object({
  ok: z.literal(false),
  error: z.string().max(64),
});
export type TelemetryError = z.infer<typeof telemetryErrorSchema>;
