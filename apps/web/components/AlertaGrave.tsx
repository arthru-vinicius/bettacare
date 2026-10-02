"use client";

import {
  COMPONENT_LABELS,
  describeEventCode,
  HEALTH_STATUS_LABELS,
  type ComponentHealth,
  type OverviewResponse,
} from "@bettacare/contract";
import { useEffect, useState } from "react";

import { formatTime } from "@/lib/format";

/**
 * Modal que aparece na abertura quando há problema grave registrado.
 *
 * A aba Diagnóstico já mostra tudo — mas mostra para quem foi procurar. Um
 * sensor que caiu às 3h da manhã fica invisível até alguém abrir a aba certa,
 * e o ponto de um alerta é justamente não depender disso.
 *
 * ## Por que ele não reaparece a cada abertura
 *
 * Um modal que volta toda vez vira algo que se fecha no reflexo, sem ler — e
 * aí ele deixou de alertar. A dispensa é gravada por **episódio**, usando o
 * `since` do componente como identidade: enquanto for o mesmo problema
 * contínuo, fica dispensado; se o componente se recuperar e falhar de novo, o
 * `since` muda e o alerta volta, porque aí é um fato novo.
 */

const CHAVE = "bettacare:alerta-dispensado";

/** Só isto interrompe: `degraded` é para olhar quando der, não agora. */
const GRAVES = new Set(["fault", "missing"]);

function assinaturaDe(data: OverviewResponse): string | null {
  if (!data.device.online) {
    // Offline é um episódio só, identificado por quando o contato caiu.
    return `offline:${data.device.last_seen_at ?? "?"}`;
  }
  const graves = data.components
    .filter((c) => GRAVES.has(c.status))
    .sort((a, b) => a.comp.localeCompare(b.comp));

  if (graves.length === 0) return null;
  return graves.map((c) => `${c.comp}:${c.status}:${c.since}`).join("|");
}

export function AlertaGrave({ data }: { data: OverviewResponse | null }) {
  const [dispensado, setDispensado] = useState<string | null>(null);

  useEffect(() => {
    try {
      setDispensado(localStorage.getItem(CHAVE));
    } catch {
      // Modo privado ou storage bloqueado: sem memória de dispensa, o alerta
      // volta a cada abertura. É o comportamento seguro — errar para o lado de
      // avisar demais, nunca para o de avisar de menos.
      setDispensado(null);
    }
  }, []);

  if (data === null) return null;

  const assinatura = assinaturaDe(data);
  if (assinatura === null || assinatura === dispensado) return null;

  const graves = data.components.filter((c) => GRAVES.has(c.status));
  const offline = !data.device.online;

  const dispensar = () => {
    try {
      localStorage.setItem(CHAVE, assinatura);
    } catch {
      // Sem storage, some só nesta sessão.
    }
    setDispensado(assinatura);
  };

  return (
    <div className="modal-fundo" role="presentation" onClick={dispensar}>
      <div
        className="modal"
        role="alertdialog"
        aria-modal="true"
        aria-labelledby="alerta-titulo"
        onClick={(e) => e.stopPropagation()}
      >
        <div className="modal-icone" aria-hidden>
          !
        </div>

        <h2 className="modal-titulo" id="alerta-titulo">
          {offline ? "Controlador sem contato" : "Problema no aquário"}
        </h2>

        {offline ? (
          <p className="modal-texto">
            O aquário parou de responder
            {data.device.last_seen_at
              ? ` desde ${formatTime(data.device.last_seen_at)}`
              : ""}
            . Verifique a energia do ESP32 e a rede Wi-Fi. Enquanto isso, ele
            continua controlando luz e ventoinha sozinho.
          </p>
        ) : (
          <p className="modal-texto">
            {graves.length === 1
              ? "Um componente precisa de atenção:"
              : `${graves.length} componentes precisam de atenção:`}
          </p>
        )}

        {graves.map((c) => (
          <LinhaGrave key={c.comp} health={c} />
        ))}

        <button className="modal-btn" onClick={dispensar} autoFocus>
          Entendi
        </button>
        <div className="modal-rodape">
          Detalhes completos na aba Diagnóstico.
        </div>
      </div>
    </div>
  );
}

function LinhaGrave({ health }: { health: ComponentHealth }) {
  const info = health.last_code ? describeEventCode(health.last_code) : undefined;

  return (
    <div className="modal-item">
      <div className="modal-item-nome">
        <span className={`health-dot ${health.status}`} />
        {COMPONENT_LABELS[health.comp]}
        <span className="modal-item-status">
          {HEALTH_STATUS_LABELS[health.status].toLowerCase()} desde{" "}
          {formatTime(health.since)}
        </span>
      </div>
      {info?.hint ? <div className="modal-item-dica">{info.hint}</div> : null}
    </div>
  );
}
