"use client";

import { useState } from "react";

import { IconBulb, IconChart, IconFan, IconHome, IconRefresh } from "@/components/icons";
import { Fan } from "@/components/screens/Fan";
import { Health } from "@/components/screens/Health";
import { Home } from "@/components/screens/Home";
import { Light } from "@/components/screens/Light";
import { Logs } from "@/components/screens/Logs";
import { Report } from "@/components/screens/Report";
import { useAppUpdate } from "@/lib/useAppUpdate";
import { useDevice } from "@/lib/useDevice";

type Aba = "inicio" | "luminaria" | "ventoinha" | "diagnostico";
type SubAba = "saude" | "logs" | "relatorios";

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

  const { data, error, loading, refreshing, feedback, refresh, send } = useDevice();
  const update = useAppUpdate();

  const grave = data?.overall === "critical" || data?.overall === "offline";
  const atencao = data?.overall === "attention";

  return (
    <div className="app">
      {update.available ? (
        <div className="banner update">
          <span>Nova versão disponível</span>
          <button onClick={update.apply}>Atualizar</button>
        </div>
      ) : null}

      {error !== null && !loading ? (
        <div className="banner offline">
          <span>Sem contato com o servidor</span>
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
            onOpenReport={() => {
              setAba("diagnostico");
              setSubAba("relatorios");
            }}
          />
        </section>

        <section className={`screen ${aba === "luminaria" ? "active" : ""}`}>
          <Light data={data} feedback={feedback} onSend={send} />
        </section>

        <section className={`screen ${aba === "ventoinha" ? "active" : ""}`}>
          <Fan data={data} feedback={feedback} onSend={send} />
        </section>

        <section className={`screen ${aba === "diagnostico" ? "active" : ""}`}>
          <div className="subtabs">
            {(
              [
                ["saude", "Saúde"],
                ["logs", "Logs"],
                ["relatorios", "Relatórios"],
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
            Montagem condicional, não CSS: as sub-abas de Logs e Relatórios
            fazem as próprias requisições, e mantê-las montadas as faria buscar
            dados que ninguém está olhando.
          */}
          {subAba === "saude" ? <Health data={data} /> : null}
          {subAba === "logs" ? <Logs /> : null}
          {subAba === "relatorios" ? <Report /> : null}
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
