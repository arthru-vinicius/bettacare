"use client";

import {
  COMPONENT_LABELS,
  describeEventCode,
  HEALTH_STATUS_LABELS,
  type ComponentHealth,
  type DeviceConfig,
  type OverviewResponse,
} from "@bettacare/contract";

import { SettingsStatus } from "@/components/SettingsStatus";
import { formatDateTime, formatTime, timeAgo } from "@/lib/format";
import type { SettingsArea, SettingsFeedback } from "@/lib/useDevice";
import { usePushNotifications, type PushState } from "@/lib/usePushNotifications";

/**
 * Saúde componente a componente.
 *
 * A ordem é fixa e coloca o hardware do aquário primeiro: é o que o usuário
 * quer ver. Infraestrutura fica embaixo porque só importa quando quebra — e
 * quando quebra, a cor a traz para a atenção sozinha.
 */
/**
 * `button` e `pot` entraram na rodada de confiabilidade de 2026-08-21
 * (UPGRADE/04, S2) — só aparecem de fato quando o firmware manda o bloco
 * `diag`; um dispositivo antigo simplesmente não os lista, sem exigir mudança
 * aqui, porque a tela renderiza o que `data.components` de fato trouxer.
 */
const ORDEM = [
  "temp",
  "fan",
  "light",
  "rtc",
  "wifi",
  "button",
  "pot",
  "api",
  "nvs",
  "ota",
  "system",
] as const;

export function Health({
  data,
  onRunDiagnostic,
  diagnosticRunning,
  settingsFeedback,
  onSaveSettings,
}: {
  data: OverviewResponse | null;
  onRunDiagnostic: () => void;
  diagnosticRunning: boolean;
  settingsFeedback: SettingsFeedback | null;
  onSaveSettings: (patch: Partial<DeviceConfig>, area: SettingsArea) => Promise<boolean>;
}) {
  if (data === null) return <div className="empty">Carregando…</div>;

  const online = data.device.online;
  const ordenados = [...data.components].sort(
    (a, b) => indice(a.comp) - indice(b.comp),
  );

  return (
    <>
      <div className="card" style={{ marginBottom: 12 }}>
        <div className="row-between">
          <div>
            <div className="health-name">{data.device.name}</div>
            <div className="health-detail">
              {data.device.fw_version ? `Firmware ${data.device.fw_version}` : "—"}
              {data.device.last_ip ? ` · ${data.device.last_ip}` : ""}
            </div>
          </div>
          <span className={`pill ${online ? "on" : "danger"}`}>
            <span className="pill-dot" />
            {online ? "ONLINE" : "OFFLINE"}
          </span>
        </div>
        <div className="c-sub">
          {online
            ? `Última telemetria ${timeAgo(data.state?.updated_at)}`
            : `Sem contato ${timeAgo(data.device.last_seen_at)}`}
        </div>
      </div>

      {!online ? (
        <div className="health-hint">
          Com o dispositivo mudo, o estado de cada componente passa a ser
          desconhecido. Manter o último valor como se ainda valesse seria
          afirmar algo que não se sabe mais.
        </div>
      ) : null}

      {ordenados.map((c) => (
        <ComponentRow key={c.comp} health={c} />
      ))}

      {ordenados.length === 0 ? (
        <div className="empty">
          Nenhuma informação de saúde ainda. Ela aparece no primeiro contato do
          dispositivo.
        </div>
      ) : null}

      {data.state?.controller ? <ControllerCard controller={data.state.controller} /> : null}

      {data.config ? (
        <FrequenciaSection
          intervaloMs={data.config.telemetry_interval_ms}
          feedback={settingsFeedback?.area === "system" ? settingsFeedback : null}
          onSave={(ms) => void onSaveSettings({ telemetry_interval_ms: ms }, "system")}
        />
      ) : null}

      <DiagnosticSection
        result={data.diagnostic}
        online={online}
        onRun={onRunDiagnostic}
        running={diagnosticRunning}
      />

      <NotificacoesSection />
    </>
  );
}

const FREQUENCIAS = [1000, 2000, 3000, 5000] as const;

/**
 * Com que frequência o aquário conversa com o servidor.
 *
 * É o que manda na demora entre tocar num botão do app e o aquário obedecer:
 * o comando espera, no máximo, um intervalo destes para ser buscado. Fica
 * aqui, e não na tela da luminária ou da ventoinha, porque é ajuste do
 * controlador — vale para tudo, e quase nunca precisa mudar.
 */
function FrequenciaSection({
  intervaloMs,
  feedback,
  onSave,
}: {
  intervaloMs: number;
  feedback: SettingsFeedback | null;
  onSave: (ms: number) => void;
}) {
  const salvando = feedback?.status === "saving" || feedback?.status === "applying";
  return (
    <>
      <div className="health-name" style={{ margin: "18px 0 6px" }}>
        Atualização do aquário
      </div>
      <div className="push-card">
        <div className="c-sub" style={{ marginTop: 0, marginBottom: 10 }}>
          De quanto em quanto tempo o aquário manda leituras e busca comandos. Mais
          rápido deixa o app mais responsivo e usa um pouco mais de rede.
        </div>
        <div className="segmentado" role="radiogroup" aria-label="Intervalo de atualização">
          {FREQUENCIAS.map((ms) => (
            <button
              key={ms}
              role="radio"
              aria-checked={intervaloMs === ms}
              className={intervaloMs === ms ? "ativo" : ""}
              disabled={salvando}
              onClick={() => {
                if (ms !== intervaloMs) onSave(ms);
              }}
            >
              {ms / 1000} s
            </button>
          ))}
        </div>
        <SettingsStatus feedback={feedback} />
      </div>
    </>
  );
}

/**
 * Ativação das notificações push.
 *
 * A permissão é pedida aqui, num botão explícito, e não na abertura do app.
 * Um navegador só pergunta uma vez: negada por reflexo, some para sempre — e
 * com ela some o único jeito de saber que algo quebrou às 3h da manhã.
 */
function NotificacoesSection() {
  const { state, ativar, desativar, testar, erro } = usePushNotifications();

  if (state === "carregando") return null;

  const textos: Record<Exclude<PushState, "carregando">, string> = {
    indisponivel:
      "Este navegador não suporta notificações. No iPhone, adicione o app à tela de início primeiro.",
    desabilitado: "O servidor está sem chaves de notificação configuradas.",
    negado:
      "Notificações bloqueadas para este site. Para reativar, mude a permissão nas configurações do navegador.",
    inativo:
      "Receba um aviso no celular ou no computador quando algo sério acontecer — mesmo com o app fechado.",
    ativo: "Você será avisado quando um componente falhar ou o controlador cair.",
  };

  return (
    <>
      <div className="health-name" style={{ margin: "18px 0 6px" }}>
        Notificações
      </div>

      <div className="push-card">
        <div className="c-sub" style={{ marginTop: 0, marginBottom: 10 }}>
          {textos[state]}
        </div>

        {state === "inativo" ? (
          <button className="push-btn" onClick={() => void ativar()}>
            Ativar notificações
          </button>
        ) : null}

        {state === "ativo" ? (
          <div className="push-linha">
            <button className="push-btn" onClick={() => void testar()}>
              Enviar teste
            </button>
            <button className="push-btn secundario" onClick={() => void desativar()}>
              Desativar
            </button>
          </div>
        ) : null}

        {erro ? (
          <div className="health-hint" style={{ marginTop: 10 }}>
            {erro}
          </div>
        ) : null}
      </div>
    </>
  );
}

/**
 * Autodiagnóstico sob demanda.
 *
 * O resto desta tela é saúde **passiva** — o servidor derivando conclusões da
 * telemetria que foi chegando. Aqui é diferente: o botão manda o dispositivo
 * ir olhar agora, sondando cada barramento, e devolver o que encontrou.
 */
function DiagnosticSection({
  result,
  online,
  onRun,
  running,
}: {
  result: OverviewResponse["diagnostic"];
  online: boolean;
  onRun: () => void;
  running: boolean;
}) {
  return (
    <>
      <div className="health-name" style={{ margin: "18px 0 6px" }}>
        Autodiagnóstico
      </div>

      <div className="diag-actions">
        <button className="btn-diag" onClick={onRun} disabled={!online || running}>
          {running ? "Executando…" : "Rodar diagnóstico agora"}
        </button>
        <div className="c-sub">
          {!online
            ? "Indisponível com o controlador sem contato."
            : result
              ? `Último: ${formatDateTime(result.ran_at)} · ${result.duration_ms} ms`
              : "Nunca executado neste dispositivo."}
        </div>
      </div>

      {result?.checks.map((c) => (
        <div key={c.comp} className="health-row">
          <span className={`health-dot ${c.status}`} />
          <div className="health-main">
            <div className="health-name">
              {COMPONENT_LABELS[c.comp]}
              {!c.probed ? <span className="diag-passive">observado</span> : null}
            </div>
            <div className="health-detail">{c.detail}</div>
          </div>
        </div>
      ))}
    </>
  );
}

type ControllerInfo = NonNullable<NonNullable<OverviewResponse["state"]>["controller"]>;

/** Nomes curtos que `esp_reset_reason()` produz — ver `api_client.cpp`. */
const RESET_REASON_LABELS: Record<string, string> = {
  poweron: "Ligado (energia)",
  ext: "Reset externo",
  sw: "Reinício por software",
  panic: "Falha grave (panic)",
  int_wdt: "Watchdog de interrupção",
  task_wdt: "Watchdog de task",
  wdt: "Watchdog",
  deepsleep: "Saiu de modo de baixo consumo",
  brownout: "Queda de tensão (brownout)",
  sdio: "Reset via SDIO",
  usb: "Reset via USB",
  jtag: "Reset via JTAG",
  efuse: "Erro de eFuse",
  pwr_glitch: "Oscilação de energia",
  cpu_lockup: "Trava de CPU",
  unknown: "Desconhecido",
};

/** Motivos que valem a pena investigar do lado elétrico — ver UPGRADE/02. */
const RESET_REASONS_DE_ATENCAO = new Set([
  "brownout",
  "panic",
  "task_wdt",
  "int_wdt",
  "wdt",
  "cpu_lockup",
]);

/**
 * Diagnóstico do **controlador**, não do aquário — acrescentado na rodada de
 * confiabilidade de 2026-08-21 (UPGRADE/06). Responde "o ESP32 reiniciou esta
 * noite, e por quê?" e "a memória está caindo?", perguntas que a lista de
 * componentes acima não tem como responder: aquilo é sobre o hardware do
 * aquário, isto é sobre o chip que o controla.
 */
function ControllerCard({ controller }: { controller: ControllerInfo }) {
  const motivo = controller.reset_reason;
  const rotuloMotivo = motivo ? (RESET_REASON_LABELS[motivo] ?? motivo) : "—";
  const atencao = motivo !== null && RESET_REASONS_DE_ATENCAO.has(motivo);

  return (
    <>
      <div className="health-name" style={{ margin: "18px 0 6px" }}>
        Controlador
      </div>

      <div className="health-row">
        <span className={`health-dot ${atencao ? "fault" : "ok"}`} />
        <div className="health-main">
          <div className="health-name">Último reinício</div>
          <div className="health-detail">{rotuloMotivo}</div>
        </div>
      </div>
      {atencao ? (
        <div className="health-hint">
          {motivo === "brownout"
            ? "Queda de tensão aponta para a alimentação: fonte, buck e o capacitor do 3,3 V."
            : "Watchdog ou falha grave aponta para o firmware: algo travou e o chip se reiniciou sozinho para se recuperar. Se repetir, vale gravar o firmware de debug e investigar."}
        </div>
      ) : null}

      <div className="health-row">
        <span className="health-dot ok" />
        <div className="health-main">
          <div className="health-name">Memória livre</div>
          <div className="health-detail">
            {controller.min_free_heap !== null
              ? `mínimo desde o boot: ${formatKb(controller.min_free_heap)}`
              : "sem histórico ainda"}
          </div>
        </div>
        <div className="health-value">
          {controller.free_heap !== null ? formatKb(controller.free_heap) : "—"}
        </div>
      </div>

      {controller.api_failures > 0 ? (
        <div className="health-row">
          <span className="health-dot degraded" />
          <div className="health-main">
            <div className="health-name">Comunicação</div>
            <div className="health-detail">
              {controller.api_failures} tentativa(s) antes do último envio aceito
            </div>
          </div>
        </div>
      ) : null}
    </>
  );
}

function formatKb(bytes: number): string {
  return `${Math.round(bytes / 1024)} KB`;
}

function ComponentRow({ health }: { health: ComponentHealth }) {
  const info = health.last_code ? describeEventCode(health.last_code) : undefined;
  const problema = health.status !== "ok" && health.status !== "unknown";
  const valor = valorDe(health);

  /*
   * Com o dispositivo offline o status vira `unknown`, mas o `detail` guarda o
   * último valor recebido. Mostrá-lo ao lado de "Sem informação" seria
   * contraditório: o número precisa vir marcado como último conhecido, e
   * apagado, para não ser lido como leitura atual.
   */
  const desatualizado = health.status === "unknown" && valor !== "";

  return (
    <>
      <div className="health-row">
        <span className={`health-dot ${health.status}`} />
        <div className="health-main">
          <div className="health-name">{COMPONENT_LABELS[health.comp]}</div>
          <div className="health-detail">
            {desatualizado
              ? `Último valor conhecido · ${formatTime(health.last_ok_at ?? health.since)}`
              : HEALTH_STATUS_LABELS[health.status]}
            {problema ? ` · desde ${formatTime(health.since)}` : ""}
            {health.status === "ok" && health.last_ok_at
              ? ` · ${timeAgo(health.last_ok_at)}`
              : ""}
          </div>
        </div>
        <div className={`health-value ${desatualizado ? "stale" : ""}`}>{valor}</div>
      </div>
      {problema && info?.hint ? <div className="health-hint">{info.hint}</div> : null}
    </>
  );
}

/** O número que faz sentido para cada componente. */
function valorDe(h: ComponentHealth): string {
  const d = h.detail;
  if (d === null || d === undefined) return "";

  switch (h.comp) {
    case "temp": {
      const c = d["celsius"];
      return typeof c === "number" ? `${c.toFixed(1)} °C` : "—";
    }
    case "fan": {
      const rpm = d["rpm"];
      const pct = d["speed_percent"];
      if (typeof rpm !== "number") return "—";
      return typeof pct === "number" && pct > 0 ? `${rpm} rpm` : "parada";
    }
    case "light": {
      const on = d["on"];
      return typeof on === "boolean" ? (on ? "acesa" : "apagada") : "—";
    }
    case "rtc": {
      const t = d["time"];
      return typeof t === "string" ? t : "—";
    }
    case "wifi": {
      const r = d["rssi"];
      return typeof r === "number" ? `${r} dBm` : "—";
    }
    case "button": {
      const pressionado = d["pressed"];
      return typeof pressionado === "boolean" ? (pressionado ? "pressionado" : "solto") : "—";
    }
    case "pot": {
      const adc = d["adc"];
      return typeof adc === "number" ? `${adc} / 4095` : "—";
    }
    default:
      return "";
  }
}

function indice(comp: string): number {
  const i = (ORDEM as readonly string[]).indexOf(comp);
  return i === -1 ? 99 : i;
}
