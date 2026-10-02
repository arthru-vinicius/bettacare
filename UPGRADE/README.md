# UPGRADE — rodada de confiabilidade e diagnóstico

Diretório de trabalho da atualização iniciada em **2026-08-20**. Tudo que
envolver esta rodada — achados, planos, decisões e ligações entre os módulos —
mora aqui.

`docs/` continua sendo a documentação do sistema **como ele foi projetado**.
`UPGRADE/` é o que muda a partir de agora. Quando uma mudança daqui for
implementada e validada, o documento correspondente em `docs/` é que deve ser
atualizado — não o contrário.

---

## O ponto de partida, sem rodeios

O sistema já foi reescrito. O monorepo, o servidor Hono, o Postgres, o firmware
HTTP e o PWA existem, estão nas tags `v1.0.0` a `v1.0.3`, e o desenho de
observabilidade em `docs/arquitetura-observabilidade.md` responde às três
perguntas certas. Isso não é pouco.

Mas há um fato que atravessa esta rodada inteira e precisa ficar dito na
primeira página:

> **Nada disto jamais rodou num ESP32 físico.**

`docs/firmware.md` já registra isso na seção "Não verificado — exige hardware":
os dois núcleos sob carga real, o tacômetro, a calibração do potenciômetro, o
OTA e o cenário de servidor fora do ar por dois dias. A validação de ponta a
ponta que existe foi feita **simulando o ESP32 com `curl`**. Ela prova que o
contrato fecha; não prova que o aquário funciona.

E há um segundo fato, este novo, levantado nesta rodada:

> **Três das quatro interfaces físicas estão fora da especificação do componente
> que controlam** — e as três falham de forma intermitente e silenciosa, que é
> exatamente a queixa que motivou o trabalho.

E um terceiro, que é o achado mais importante da auditoria:

> **Existe um caminho de falha que liga um resistor de R$ 0,10 ausente a uma pane
> total de telemetria** — e ele apaga o próprio rastro. Está desenhado em
> [01](01-achados.md#o-achado-que-importa-mais).

Os três fatos juntos mudam a prioridade. A queixa original — *"componentes
paravam de funcionar com frequência, não havia logs, e às vezes ele
simplesmente não funcionava"* — foi tratada até aqui como um problema de
software, e a resposta foi observabilidade. Observabilidade é necessária: sem
ela não se sabe o que está quebrado. Mas ela **não conserta** um SSR que recebe
metade da tensão de que precisa.

A ordem certa é hardware primeiro, instrumentação junto, e o resto depois.

---

## Documentos

| # | Documento | O que contém |
|---|---|---|
| 01 | [Relatório de achados](01-achados.md) | Síntese das cinco camadas e o que atravessa mais de uma. **Comece por aqui** |
| 02 | [Hardware, ligações e componentes](02-hardware-e-ligacoes.md) | Análise elétrica, correções, lista de compras e roteiro de bancada |
| 03 | [Plano de ação — firmware](03-plano-firmware.md) | 14 achados no firmware do ESP32 |
| 04 | [Plano de ação — servidor e banco](04-plano-servidor-e-banco.md) | 14 achados, com DDL concreta para a telemetria de diagnóstico |
| 05 | [Plano de ação — contrato e comunicação](05-plano-contrato-e-comunicacao.md) | 6 achados, incluindo o mais grave da auditoria |
| 06 | [Plano de ação — PWA](06-plano-pwa.md) | 6 achados. A camada mais saudável do projeto |
| 07 | [Roteiro de execução](07-roteiro-de-execucao.md) | Fases, dependências e critérios de aceite |

Cada documento por camada é autocontido: traz os achados, o que está certo e não
deve ser mexido, e o plano de ação com dependências. O 01 não repete o conteúdo
deles — ele conecta.

Os documentos nasceram como pacotes de trabalho para agentes especializados,
com contexto suficiente para trabalhar sem reabrir as decisões.

> **Situação em 2026-10-01:** o plano de firmware (03) está implementado — só o
> F8 ficou de fora, de propósito, à espera de dado de fragmentação. A montagem
> em bancada que veio depois achou e corrigiu o que nenhum plano previa (o relé
> trocando sozinho, um GPIO queimado, o watchdog que não valia), e está
> registrada no fim do [07](07-roteiro-de-execucao.md#rodada-de-bancada--2026-10-01).
> Os demais planos não foram reconferidos nesta revisão.

---

## O elo entre as duas frentes

Vale registrar, porque não é óbvio e é o que evita tratar as frentes como
independentes: **a instrumentação nova é o instrumento de medida das hipóteses
de hardware.**

O caso mais claro é o reinício por brownout. A suspeita de que falta capacitor
de reservatório no trilho de 3,3 V é, hoje, opinião fundamentada. Basta o
firmware reportar `esp_reset_reason()` na telemetria para ela virar medição:

- reinícios por **brownout** → o problema é elétrico, e a correção está no
  documento 02;
- reinícios por **task watchdog** → o problema é software, e a correção está no
  plano de firmware;
- **nenhum reinício** → a hipótese cai, e a busca vai para outro lugar.

O mesmo vale para o tacômetro, para o SSR e para o 1-Wire. Por isso os campos de
diagnóstico do firmware (`reset_reason`, heap livre e mínimo, RSSI, uptime,
contadores de reconexão) valem ser priorizados **acima** de quase todo o resto do
plano de telemetria: eles são baratos, cabem no payload, e são o que transforma
as próximas semanas de depuração em algo dirigido por dado.

---

## Regras desta rodada

Herdadas de `docs/` e de decisões já fechadas. Estão aqui para não serem
reabertas por engano:

- **Nada de MQTT, PHP, Redis, MySQL, Nginx/Caddy, Watchtower ou
  `network_mode: host`.** O contrato do homelab
  (`docs/requisitos-bettacare.md`) proíbe explicitamente.
- **O hardware é o que existe:** luminária por SSR, ventoinha de 4 fios com
  tacômetro, DS18B20, DS3231, botão e potenciômetro. Não há bomba nem
  aquecedor, ainda que o documento do homelab os mencione.
- **Comandos são estado desejado**, nunca *toggle*. Um *toggle* reentregue
  inverte o estado duas vezes.
- **`packages/contract` é a fonte única do contrato.** Divergência entre as três
  pontas foi a classe de bug que motivou o pacote existir.
- **Nunca a tag `latest`.** Publicar é decisão explícita, por tag semver.
- **Testar em bancada, com o ESP32 no USB, antes de gravar no que está no
  aquário.** Se o firmware novo quebrar a rede, o OTA não salva.
