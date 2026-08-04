# Plano de migração — smart-aquarium-system → BettaCare

Companheiro de `respostas-bettacare.md`, que responde ao agente do servidor.
Este aqui é o roteiro da nossa parte.

---

## Decisões tomadas

| Decisão | Escolha |
|---|---|
| Repositório | novo repo `bettacare`, monorepo, firmware junto |
| Servidor | Hono servindo o export estático do Next — **um processo, duas portas** |
| ESP32 ↔ servidor | `POST /api/v1/telemetry` a cada 3 s, comandos na resposta |
| Escopo de hardware | só o que existe: luminária, ventoinha com RPM, temperatura |

O repositório `smart-aquarium-system` fica **parado como legado**. Não vamos
migrar commits: ele continua sendo a referência de comportamento (a máquina de
estados da ventoinha vale ouro), mas não é código a evoluir.

---

## Estrutura do monorepo

```
bettacare/
├─ apps/
│  ├─ web/                  ← Next.js, output: 'export' — PWA instalável
│  └─ server/               ← Hono: serve o export + API + ingest do ESP32
├─ packages/
│  └─ contract/             ← tipos e schemas Zod do contrato ESP32↔servidor
├─ firmware/
│  └─ bettacare/            ← sketch Arduino (pasta = nome do .ino)
├─ .github/workflows/
│  └─ release.yml           ← build multi-stage + push ghcr.io em git tag
├─ Dockerfile
└─ docs/
```

`packages/contract` é o detalhe que evita a classe de bug mais chata deste
projeto: o corpo do POST é definido **uma vez**, em Zod. O servidor valida com
ele, o front tipa com ele, e o `.h` do firmware é **gerado** a partir dele por um
script. Hoje o formato do JSON está escrito à mão em três lugares
(`web_server.cpp`, `mqtt_manager.cpp`, `app.html`) e eles já divergem entre si.

---

## O que vale a pena salvar do código atual

Vale portar quase literalmente:

- **`fan.cpp`** — histerese, cooldown de 30 min, escalonamento progressivo,
  failsafe de temperatura indisponível e calibração do potenciômetro. É a lógica
  mais madura do projeto e não tem nada a ver com o transporte. Muda só de onde
  vêm os limiares.
- **`rtc_manager.cpp`** — a regra de "só age na transição de período, override
  manual sobrevive dentro da janela" é sutil e está correta.
- **`temperature.cpp`** — leitura não-bloqueante com noção de leitura *stale*.
- **`wifi_manager.cpp`** — AP de recuperação + credenciais em NVS.
- **A UI** como referência visual: as quatro telas, o relógio sincronizado com o
  RTC, o slider que só habilita com a ventoinha ligada. Reescrever em React, não
  reinventar o desenho.

Morre inteiro: `mqtt_manager.*`, todo o `server/` em PHP, e o `log_manager` no
formato atual (buffer circular de 30 entradas em RAM vira tabela `events`).

---

## Fases

### Fase 1 — Contrato e banco

Definir `packages/contract` e o schema Drizzle antes de escrever qualquer
endpoint. É o que trava a arquitetura; todo o resto decorre daí.

Entregável: migrations rodando contra um Postgres local em Docker.

### Fase 2 — Servidor

Hono com os dois roteadores, `/healthz`, ingest com `timingSafeEqual`,
idempotência por `(device_id, boot_id, seq)`, fila de comandos com ack.

Entregável: dá para simular o ESP32 inteiro com `curl` e ver o estado no banco.
**Esta fase termina antes de encostar no firmware** — assim o firmware é
desenvolvido contra um servidor que já funciona.

### Fase 3 — Firmware

Ver a seção seguinte. É a fase de maior risco, porque um erro aqui exige
recuperação física do dispositivo.

### Fase 4 — Interface

Next.js com export estático, PWA (`app/manifest.ts` + service worker em
`public/`), as quatro telas, mais a tela nova que o banco viabiliza: **relatórios**
— curva de temperatura, horas de luz por dia, tempo de ventoinha ligada.

Era o que não dava para fazer com MQTT retido, e é a razão de o banco existir.

### Fase 5 — CI e entrega

`release.yml` disparado por tag `v*`, build `linux/amd64`, push para
`ghcr.io/arthru-vinicius/bettacare:v1.0.0`. Nunca `latest`.

Aí sim entregar `respostas-bettacare.md` ao agente do servidor.

---

## O firmware

### O que muda

- `mqtt_manager.*` → `api_client.*`: um `POST` a cada 3 s, resposta traz comandos
- Configuração passa a ter o servidor como fonte de verdade; NVS vira cache
  offline, sincronizado por `config_version`
- `log_manager` deixa de ser buffer circular publicado por MQTT e passa a
  acumular eventos até o próximo POST, que os drena
- `boot_id` (contador em NVS, incrementado no boot) + `seq` monotônico, para a
  idempotência do servidor
- `light.toggle` deixa de existir. Comandos passam a ser **estado desejado**
  (`light.set{on:true}`), porque reentrega de toggle inverte duas vezes

### O que precisa ser corrigido de qualquer forma

O loop atual é single-threaded com `delay(200)`, e o handshake TLS do MQTT
bloqueia até 6 s. Ou seja: **hoje, uma instabilidade de rede trava o botão
físico e a histerese da ventoinha**. Trocar MQTT por HTTP não resolve isso
sozinho — HTTP também bloqueia.

Na reescrita, a rede vai para uma **task FreeRTOS própria** (core 0), e o
controle local fica no loop principal (core 1), comunicando por fila. O aquário
passa a funcionar com a mesma confiabilidade com ou sem servidor.

### Higiene

O `config.example.h` do repositório antigo trazia uma senha de OTA real no lugar
de um placeholder. Aqui o template foi sanitizado, mas **essa credencial precisa
ser rotacionada no dispositivo**: ela esteve versionada e deve ser considerada
comprometida.

---

## Riscos e pontos de atenção

**O documento do servidor menciona bomba e aquecedor** (seção 4.1), que não
existem no hardware. Decidimos modelar só o que existe. Vale avisar o agente do
homelab para ele não provisionar supondo mais atuadores — e, se você pretende
adicionar aquecedor depois, é uma migration, não uma reescrita.

**O ESP32 vira dependente da LAN para configuração.** Hoje ele é autossuficiente:
os horários vivem no NVS. O desenho com `config_version` preserva a operação
offline, mas é preciso testar de verdade o cenário "servidor fora do ar por dois
dias" antes de considerar a migração concluída.

**Janela de indisponibilidade.** Enquanto o firmware novo não estiver validado, o
aquário continua no sistema antigo. Recomendo manter o HiveMQ ativo até a Fase 3
terminar, e só então desligar a conta.

**Recuperação.** O ElegantOTA continua sendo a via de atualização, mas se o
firmware novo quebrar a rede, OTA não salva. Testar em bancada, com o ESP32 no
USB, antes de gravar no que está no aquário.
