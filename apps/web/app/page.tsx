"use client";

import { useState } from "react";

import { AlertaGrave } from "@/components/AlertaGrave";
import { Carregando } from "@/components/Carregando";
import { IconBulb, IconChart, IconFan, IconFeeder, IconHome, IconRefresh } from "@/components/icons";
import { Fan } from "@/components/screens/Fan";
import { Feeder } from "@/components/screens/Feeder";
import { Health } from "@/components/screens/Health";
import { Home } from "@/components/screens/Home";
import { Light } from "@/components/screens/Light";
import { Logs } from "@/components/screens/Logs";
import { formatTime } from "@/lib/format";
import { useAppUpdate } from "@/lib/useAppUpdate";
import { useDevice } from "@/lib/useDevice";

type Aba = "inicio" | "luminaria" | "ventoinha" | "alimentador" | "diagnostico";
/**
 * Duas, não três: Logs e Relatórios respondiam a mesma pergunta ("o que
 * aconteceu nesse período?") em telas separadas, e Relatórios não deixava
 * levar nada embora. Agora é "Registros", com a planilha no mesmo lugar. O
 * gráfico de temperatura foi para a tela da ventoinha, junto da temperatura.
 */
type SubAba = "saude" | "registros";

/**
 * O casco do app.
 *
 * Uma página só, com as telas trocadas por estado do cliente — como no app
 * antigo. Com `output: 'export'` isso também simplifica o service worker: há
 * um único HTML a manter em cache, e nenhuma navegação a interceptar.
 */
export default function Page() {
  const [aba, setAba] = useState<Aba>("inicio");
  const [subAba, setSubAba] = useState<SubAba>("saude");

  const {
    data,
    error,
    deviceMissing,
    loading,
    refreshing,
    feedback,
    refresh,
    send,
    runDiagnostic,
    diagnosticRunning,
    saveSettings,
    settingsFeedback,
  } = useDevice();
  const update = useAppUpdate();

  const grave = data?.overall === "critical" || data?.overall === "offline";
  const atencao = data?.overall === "attention";

  // Só na primeira carga. Uma atualização de polling não pode jogar a tela de
  // abertura por cima de quem está no meio de alguma coisa — para isso existe
  // o `refreshing`, que anima só o botão.
  if (loading) return <Carregando />;

  return (
    <div className="app">
      <AlertaGrave data={data} />

      {update.available ? (
        <div className="banner update">
          <span>Nova versão disponível</span>
          <button onClick={update.apply}>Atualizar</button>
        </div>
      ) : null}

      {/*
        Três causas distintas, três mensagens. Confundi-las na primeira versão
        fez a interface acusar o servidor por um problema do aquário — e a
        pessoa lendo iria depurar a peça errada.

        A ordem importa: se não há contato com o servidor, nada se sabe sobre o
        aquário, então essa mensagem vem primeiro e as outras nem aparecem.
      */}
      {error !== null ? (
        <div className="banner offline">
          <span>Sem contato com o servidor</span>
        </div>
      ) : deviceMissing ? (
        <div className="banner update">
          <span>Aguardando o primeiro contato do aquário</span>
        </div>
      ) : data !== null && !data.device.online ? (
        <div className="banner offline">
          <span>
            Sem contato com o controlador do aquário
            {data.device.last_seen_at
              ? ` desde ${formatTime(data.device.last_seen_at)}`
              : ""}
          </span>
        </div>
      ) : null}

      <header className="header">
        <div className="brand">
          <div className="brand-icon">
            {/* eslint-disable-next-line @next/next/no-img-element */}
            <img src="/icons/icon-192.png" alt="" />
          </div>
          <span className="brand-name">BettaCare</span>
        </div>
        <div className="header-right">
          <button
            className={`btn-icon ${refreshing ? "spinning" : ""}`}
            onClick={() => void refresh()}
            aria-label="Atualizar"
          >
            <IconRefresh />
          </button>
        </div>
      </header>

      <div className="screens">
        <section className={`screen ${aba === "inicio" ? "active" : ""}`}>
          <Home
            data={data}
            deviceMissing={deviceMissing}
            onOpenFan={() => setAba("ventoinha")}
            onOpenLight={() => setAba("luminaria")}
          />
        </section>

        <section className={`screen ${aba === "luminaria" ? "active" : ""}`}>
          <Light
            data={data}
            feedback={feedback}
            onSend={send}
            settingsFeedback={settingsFeedback}
            onSaveSettings={saveSettings}
          />
        </section>

        <section className={`screen ${aba === "ventoinha" ? "active" : ""}`}>
          <Fan
            data={data}
            feedback={feedback}
            onSend={send}
            settingsFeedback={settingsFeedback}
            onSaveSettings={saveSettings}
          />
        </section>

        <section className={`screen ${aba === "alimentador" ? "active" : ""}`}>
          <Feeder data={data} feedback={feedback} onSend={send} />
        </section>

        <section className={`screen ${aba === "diagnostico" ? "active" : ""}`}>
          <div className="subtabs">
            {(
              [
                ["saude", "Saúde"],
                ["registros", "Registros"],
              ] as const
            ).map(([id, label]) => (
              <button
                key={id}
                className={`subtab ${subAba === id ? "active" : ""}`}
                onClick={() => setSubAba(id)}
              >
                {label}
              </button>
            ))}
          </div>

          {/*
            Montagem condicional, não CSS: Registros faz as próprias
            requisições, e mantê-la montada a faria buscar dados que ninguém
            está olhando.
          */}
          {subAba === "saude" ? (
            <Health
              data={data}
              onRunDiagnostic={() => void runDiagnostic()}
              diagnosticRunning={diagnosticRunning}
              settingsFeedback={settingsFeedback}
              onSaveSettings={saveSettings}
            />
          ) : null}
          {subAba === "registros" ? <Logs /> : null}
        </section>
      </div>

      <nav className="nav">
        <NavBtn ativo={aba === "inicio"} onClick={() => setAba("inicio")} label="Início">
          <IconHome />
        </NavBtn>
        <NavBtn
          ativo={aba === "luminaria"}
          onClick={() => setAba("luminaria")}
          label="Luminária"
        >
          <IconBulb />
        </NavBtn>
        <NavBtn
          ativo={aba === "ventoinha"}
          onClick={() => setAba("ventoinha")}
          label="Ventoinha"
        >
          <IconFan />
        </NavBtn>
        <NavBtn
          ativo={aba === "alimentador"}
          onClick={() => setAba("alimentador")}
          label="Alimentador"
        >
          <IconFeeder />
        </NavBtn>
        <NavBtn
          ativo={aba === "diagnostico"}
          onClick={() => setAba("diagnostico")}
          label="Diagnóstico"
          /* O ponto na aba é o que faz um problema ser notado sem o usuário ir procurar. */
          badge={grave ? "danger" : atencao ? "warn" : null}
        >
          <IconChart />
        </NavBtn>
      </nav>
    </div>
  );
}

function NavBtn({
  ativo,
  onClick,
  label,
  badge,
  children,
}: {
  ativo: boolean;
  onClick: () => void;
  label: string;
  badge?: "danger" | "warn" | null;
  children: React.ReactNode;
}) {
  return (
    <button className={`nav-btn ${ativo ? "active" : ""}`} onClick={onClick}>
      {children}
      <span className="nav-label">{label}</span>
      {badge ? <span className={`nav-badge ${badge === "warn" ? "warn" : ""}`} /> : null}
    </button>
  );
}
