import { DEFAULT_COMMAND_MAX_AGE_S } from "@bettacare/contract";
import { z } from "zod";

/**
 * Toda a configuração vem de variável de ambiente — exigência do contrato do
 * homelab, e a razão de o mesmo artefato rodar em qualquer host.
 *
 * Não existe nenhuma variável de *build time*: a interface é um export estático
 * que fala com a própria origem por caminho relativo, então não há
 * `NEXT_PUBLIC_*` com host ou segredo para congelar na imagem.
 */
const envSchema = z.object({
  DATABASE_URL: z.string().min(1, "DATABASE_URL é obrigatória"),
  /** Valor esperado no header `X-Api-Token` do ESP32. */
  DEVICE_INGEST_TOKEN: z.string().min(16, "o token de ingestão é curto demais"),

  /** UI + API do navegador. Não publicada: o cloudflared alcança por DNS interno. */
  PORT_UI: z.coerce.number().int().min(1).max(65535).default(3000),
  /** Ingest do ESP32. Publicada com bind explícito no IP da LAN. */
  PORT_INGEST: z.coerce.number().int().min(1).max(65535).default(8080),

  NODE_ENV: z.enum(["development", "production", "test"]).default("production"),
  LOG_LEVEL: z
    .enum(["trace", "debug", "info", "warn", "error", "fatal"])
    .default("info"),

  /**
   * Onde está o export estático do Next.
   *
   * Vazio em desenvolvimento: o caminho é resolvido relativo ao módulo, o que
   * funciona porque a árvore do repositório é conhecida. Na imagem Docker o
   * layout é outro, e um caminho relativo fixo levaria a uma UI respondendo
   * 503 sem nenhuma pista do motivo — então lá ele é declarado.
   */
  WEB_ROOT: z.string().default(""),

  /** Só para apresentação. O banco é sempre UTC. */
  TZ_DISPLAY: z.string().default("America/Recife"),
  /** Pequeno de propósito: três usuários e um dispositivo. */
  PG_POOL_MAX: z.coerce.number().int().min(1).max(20).default(4),

  /**
   * Teto de tamanho do banco. Ao ser excedido, a purga derruba as partições
   * mensais mais antigas de `telemetry` e `events`. Padrão: 1 GiB.
   */
  DB_SIZE_LIMIT_BYTES: z.coerce
    .number()
    .int()
    .min(64 * 1024 * 1024)
    .default(1024 * 1024 * 1024),

  /**
   * Por quanto tempo guardar o histórico de rodadas da ventoinha (`fan_runs`).
   * O app mostra até 30 dias; 90 dá folga para olhar um mês contra o outro. O
   * que passa disso é apagado na manutenção diária e o espaço é reaproveitado —
   * a tabela fica do mesmo tamanho em vez de crescer com a idade da instalação.
   */
  FAN_RUNS_RETENTION_DAYS: z.coerce.number().int().min(7).max(3650).default(90),
  /** Teto de linhas de `fan_runs` por dispositivo, contra uma ventoinha oscilando. */
  FAN_RUNS_MAX_ROWS: z.coerce.number().int().min(100).default(20_000),

  /** Silêncio a partir do qual o dispositivo é dado como offline. */
  DEVICE_OFFLINE_AFTER_S: z.coerce.number().int().min(10).default(60),
  /**
   * Silêncio a partir do qual o offline vira **notificação no celular**.
   *
   * Separado de `DEVICE_OFFLINE_AFTER_S` de propósito: o Wi-Fi do aquário cai
   * por 2 a 5 minutos várias vezes ao dia (mediana de 2,3 min em produção,
   * 2026-10), e com o push saindo a cada queda foram 103 notificações em uma
   * semana — o tipo de alarme que se aprende a ignorar. A aba Saúde e os logs
   * continuam marcando offline em 60 s; o celular só toca quando o silêncio
   * já é um problema de verdade.
   */
  DEVICE_OFFLINE_ALERT_AFTER_S: z.coerce.number().int().min(60).default(900),
  /**
   * Prazo de vida de um comando, contado da criação — ver
   * `DEFAULT_COMMAND_MAX_AGE_S` no contrato, de onde vem o padrão e o motivo.
   */
  COMMAND_MAX_AGE_S: z.coerce
    .number()
    .int()
    .min(60)
    .max(24 * 60 * 60)
    .default(DEFAULT_COMMAND_MAX_AGE_S),
  /**
   * **Obsoleta desde a v1.3.0**, aceita só para não quebrar o compose que
   * ainda a declara. Era o prazo de 90 s contado da primeira entrega; o
   * comando agora vive até `COMMAND_MAX_AGE_S`.
   */
  COMMAND_TTL_S: z.coerce.number().int().min(10).optional(),

  /**
   * Par VAPID do Web Push. **Opcionais de propósito**: sem eles, o servidor
   * gera um par na primeira subida e o persiste em `server_keys`, para as
   * notificações funcionarem sem ninguém ter de editar o compose. Declará-los
   * só faz sentido para quem quer controlar as chaves fora do banco.
   */
  VAPID_PUBLIC_KEY: z.string().default(""),
  VAPID_PRIVATE_KEY: z.string().default(""),
  /** Contato exigido pela especificação do Web Push; não precisa ser monitorado. */
  VAPID_CONTACT_EMAIL: z.string().default("bettacare@localhost"),

  /**
   * Defesa em profundidade opcional. O Cloudflare Access já barra quem não
   * deve entrar; vazia, a aplicação confia inteiramente nele.
   */
  ALLOWED_USER_EMAILS: z
    .string()
    .default("")
    .transform((s) =>
      s
        .split(",")
        .map((e) => e.trim().toLowerCase())
        .filter(Boolean),
    ),
});

export type AppConfig = Readonly<z.infer<typeof envSchema>>;

export function loadConfig(env: NodeJS.ProcessEnv = process.env): AppConfig {
  const parsed = envSchema.safeParse(env);
  if (!parsed.success) {
    const problemas = parsed.error.issues
      .map((i) => `  ${i.path.join(".")}: ${i.message}`)
      .join("\n");
    throw new Error(`Configuração inválida:\n${problemas}`);
  }
  return Object.freeze(parsed.data);
}
