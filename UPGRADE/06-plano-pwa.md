# PWA — achados e plano de ação

Auditoria de `apps/web/**`.

**Veredito geral: você está certo, o PWA está bem.** É a camada mais saudável do
projeto. Não há nada aqui que justifique reescrita, e a maior parte do trabalho
desta rodada não passa por ele.

Achei **um bug real**, **duas lacunas em relação ao desenho** e alguns ajustes
pequenos. O resto do documento é sobre onde encaixar as métricas novas quando
elas chegarem.

---

## Sumário dos achados

| # | Achado | Onde | Gravidade |
|---|---|---|---|
| P1 | O polling rápido nunca acontece — `POLL_FAST_MS` é código morto | `lib/useDevice.ts:129-141` | **Média** |
| P2 | Busca por texto nos logs não existe no front | `lib/api.ts:46-65` | **Média** |
| P3 | "Servidor sem banco de dados" não é distinguido | `lib/useDevice.ts:212` | **Baixa** |
| P4 | Comando para dispositivo offline fica girando para sempre | `lib/useDevice.ts:96` | **Média** (causa no servidor) |
| P5 | `CommandStatus` sem `aria-live` | `components/CommandStatus.tsx:12` | **Baixa** |
| P6 | Sem recuo quando o servidor está fora | `lib/useDevice.ts:134` | **Baixa** |

---

## Os achados em detalhe

### P1 — O polling rápido é código morto

**Média**, e é o único bug de verdade do front.

`lib/useDevice.ts:16-18` define dois ritmos:

```ts
const POLL_MS = 4000;
/** Enquanto há comando em voo, acompanha de perto para o desfecho aparecer logo. */
const POLL_FAST_MS = 1500;
```

E `start()` escolhe entre eles (`:131-135`):

```ts
const start = () => {
  if (timer !== null) return;
  const ms = watching.current.size > 0 ? POLL_FAST_MS : POLL_MS;
  timer = setInterval(() => void load(), ms);
};
```

O problema é *quando* `start()` roda. O `useEffect` que a contém depende de
`[load]` (`:161`), e `load` é um `useCallback` sem dependências — estável para
sempre. O efeito roda **uma vez**, na montagem.

Enviar um comando faz `watching.current.add(id)` (`:175`), mas `watching` é um
`useRef`: alterá-lo não dispara re-render nem re-executa o efeito. O intervalo já
criado continua em 4.000 ms.

Resultado: **o ramo `POLL_FAST_MS` só é alcançado se o usuário esconder e
reexibir a aba enquanto um comando está em voo** — porque só aí `stop()` e
`start()` rodam de novo. No fluxo normal, nunca.

O efeito prático é pequeno — o desfecho aparece em até 4 s em vez de 1,5 s — mas
é exatamente o momento em que a resposta importa: o usuário acabou de tocar num
botão e está olhando para ele.

**Correção:** promover `watching` a estado, ou reiniciar o intervalo
explicitamente dentro de `send()`. A segunda é menor e não mexe no ciclo de
renderização.

### P2 — A busca por texto dos logs não foi implementada no front

**Média.** A seção 7 de `docs/arquitetura-observabilidade.md` descreve a aba
Logs assim:

> chips de severidade (Tudo / Erros / Avisos / Info), seletor de componente e
> **busca por texto**.

E `docs/api-servidor.md` documenta o parâmetro no servidor:

| `q` | `sensor` | busca em `msg` e `code` |

O `EventQuery` do cliente (`lib/api.ts:46-52`) tem `sev`, `comp`, `source`,
`cursor` e `limit`. **Não tem `q`**, e `components/screens/Logs.tsx` não tem
campo de busca.

Ou seja: o servidor sabe buscar e ninguém pede. É a lacuna de maior valor por
esforço no front inteiro — o parâmetro já existe, falta um `<input>` e uma
linha no `URLSearchParams`.

Vale mais do que parece depois desta rodada: com os 22 códigos novos do
[catálogo](05-plano-contrato-e-comunicacao.md) e os campos de diagnóstico, a
lista de logs fica bem mais densa, e filtrar por severidade e componente deixa
de bastar.

### P3 — "Servidor sem banco de dados" não é distinguido

**Baixa.** O documento de observabilidade lista quatro frases para o desfecho de
um comando. Três estão implementadas em `describeFailure`
(`lib/useDevice.ts:212-245`) e estão bem feitas — em especial a distinção entre
"expirou com o dispositivo offline" e "expirou com ele online", que é sutil e
está correta.

A quarta — *"Servidor sem banco de dados"* — não existe. O servidor responde
`503` com `{"error":"db_unavailable"}`, e `lib/api.ts:38` transforma qualquer
resposta não-ok num `ApiError` genérico. A mensagem que chega ao usuário é "Não
foi possível falar com o servidor" (`:183`), que aponta para a rede quando o
problema é o banco.

É o mesmo tipo de erro de atribuição que o comentário em `lib/useDevice.ts:49-56`
descreve ter sido um bug real antes — apontar o dedo para a peça que está
funcionando.

**Correção:** ler o campo `error` do corpo no `ApiError` e tratar
`db_unavailable` explicitamente. Duas linhas em `api.ts`, uma em `useDevice.ts`.

### P4 — O aviso "enviando" gira para sempre quando o dispositivo está offline

**Média**, mas **a causa não está aqui.**

`lib/useDevice.ts:95-116` resolve o feedback quando o comando chega a `acked`,
`rejected` ou `expired`. Enquanto o status for `queued` ou `sent`, o aviso
permanece `pending` — e `CommandStatus` mostra o spinner "Enviando ao
dispositivo… Ele busca a fila a cada 3 segundos."

Com o dispositivo offline, o comando fica em `queued` e, pelo achado S4 do
[plano de servidor](04-plano-servidor-e-banco.md), **`queued` nunca expira**.
O spinner gira indefinidamente, prometendo uma entrega em 3 segundos que não vai
acontecer.

Ironicamente, a frase certa já existe no código (`:234`): *"Dispositivo sem
contato desde HH:MM"*. Ela só nunca é alcançada, porque depende de o comando
chegar a `expired`.

**Correção:** primária no servidor (S4). No front, vale uma salvaguarda — se
`data.device.online` for falso e o feedback estiver `pending` há mais de alguns
segundos, trocar a mensagem sem esperar o servidor. O dado para isso já está em
`data`.

### P5 e P6 — Ajustes pequenos

**P5** — `components/CommandStatus.tsx` não tem `role="status"` nem
`aria-live="polite"`. O componente muda de "Enviando…" para "Confirmado" ou para
uma mensagem de erro **sem nenhuma interação do usuário**, que é precisamente o
caso em que um leitor de tela precisa ser avisado. `components/Carregando.tsx:16`
já faz isso certo — é só replicar. Uma linha.

**P6** — `lib/useDevice.ts:134` mantém o intervalo fixo mesmo com o servidor
fora. Um app deixado aberto num tablet ao lado do aquário faz 900 requisições
falhas por hora. Não quebra nada — é servidor local — mas polui o log do
servidor e gasta bateria. Um recuo simples (dobrar o intervalo a cada falha, teto
de 60 s, voltar ao normal no primeiro sucesso) resolve. O firmware já faz
exatamente isso em `net_task.cpp:54-73`.

---

## O que está certo e não deve ser mexido

O front acerta várias coisas difíceis, e vale registrá-las:

- **O ciclo de vida do comando na interface.** `CommandStatus` implementa
  literalmente o que a seção 4 do documento de observabilidade pede: o botão não
  volta ao normal sozinho, passa por *enviando → confirmado* ou *enviando →
  falhou com a razão*. E a razão vem de `describeEventCode()`, ou seja, **do
  catálogo do contrato, não de heurística no front** — que era o requisito
  explícito.
- **`CommandFeedback.target`** (`lib/useDevice.ts:28`). Sem ele, uma falha da
  luminária apareceria também na tela da ventoinha. É o tipo de detalhe que só
  aparece usando.
- **`deviceMissing` separado de `error`** (`:49-57`). O comentário conta que
  confundir os dois foi um bug real: com o ESP32 ainda sem ter feito o primeiro
  POST, o app anunciava "sem contato com o servidor" e culpava a peça que estava
  funcionando. A distinção está certa e é sutil.
- **O polling para quando a aba está oculta** (`:143-153`), com `AbortController`
  e remoção do listener na limpeza. Sem vazamento.
- **O service worker inteiro.** Três estratégias, cada uma pela razão certa:
  cache-first para `/_next/static/**` (nome com hash, conteúdo imutável),
  network-first para navegação (senão o usuário fica preso numa build velha), e
  stale-while-revalidate para o resto. `/api/*` e `/version.json` fora do cache —
  e a linha 68 é o que faz o mecanismo de atualização existir.
- **Não chamar `skipWaiting()` no `install`** (`public/sw.js:35-38`). Trocar por
  baixo dos pés de quem está no meio de uma ação é como se perde estado. A troca
  é decisão do usuário, ou da regra de `minBuild`.
- **O gráfico de temperatura em SVG puro** (`components/screens/Report.tsx`),
  com banda de mínimo/máximo e linha de média. Sem biblioteca de gráficos, o que
  para quatro telas é a escolha certa.
- **Caminhos relativos e nenhuma `NEXT_PUBLIC_*` com host** (`lib/api.ts:9-19`).
  É o que faz o mesmo artefato rodar em qualquer host.

---

## Onde encaixar as métricas novas

Quando o bloco `diag` do
[plano de contrato](05-plano-contrato-e-comunicacao.md) chegar, a pergunta é
onde exibi-lo sem inchar as telas. A resposta curta: **quase tudo no lugar que já
existe.**

**Aba Saúde — nada muda estruturalmente.** `Health.tsx` já lê `detail` dos
componentes e sabe exibir valor corrente (`:136` lê `rssi` de dentro do
`detail`). As duas linhas novas — `button` e `pot` — entram sozinhas assim que o
servidor passar a emiti-las, porque a tela renderiza a lista que vem da API.

Falta apenas acrescentá-las a `AQUARIUM_COMPONENTS`
(`packages/contract/src/primitives.ts:30-36`), que hoje lista cinco: `temp`,
`fan`, `light`, `rtc`, `wifi`. **É uma linha no contrato, e ela é o que faz as
duas aparecerem.**

**Aba Saúde, detalhe do componente — é onde o diagnóstico do controlador cabe.**
Um cartão novo "Controlador", separado dos componentes do aquário, com
`reset_reason`, uptime, heap livre e mínimo, e contador de reconexões. Ele
responde "o ESP32 está saudável?", que é uma pergunta diferente de "o aquário
está saudável?" — e misturar as duas foi o que tornou o log antigo inútil.

**Aba Relatórios — uma série nova.** `Report.tsx` já tem a mecânica de desenhar
uma curva a partir de `telemetry_hourly`. Heap livre ao longo dos dias usa
exatamente o mesmo componente, e é o gráfico que confirma ou descarta a hipótese
de fragmentação (F8 do firmware). Vale como aba secundária, não na primeira tela.

**Aba Logs — o campo de busca do P2**, que passa a valer mais com o volume novo.

**O que não fazer:** não colocar heap nem RSSI na tela Início. Ela responde "está
tudo bem com o aquário?", e encher de número de diagnóstico transforma a resposta
numa planilha. O hub de Diagnóstico existe exatamente para separar as duas
audiências.

---

## Plano de ação

O front é a **última** frente a mexer, porque quase tudo o que ele ganha depende
de dado que ainda não chega.

### Etapa 1 — Independente das outras frentes (P1, P2, P3, P5, P6)

Pode ser feita a qualquer momento, não depende de firmware, servidor nem
contrato.

1. **P1** — reiniciar o intervalo em `send()`, para o polling rápido existir.
2. **P2** — campo de busca na aba Logs, ligado ao `q` que o servidor já aceita.
3. **P3** — distinguir `db_unavailable` no `ApiError`.
4. **P5** — `role="status"` e `aria-live="polite"` no `CommandStatus`.
5. **P6** — recuo progressivo no polling.

**Critério de aceite:** derrubar o Postgres com a interface aberta; a mensagem
exibida fala em banco de dados, não em rede.

### Etapa 2 — Depois do servidor (P4)

1. Salvaguarda do "enviando" indefinido, uma vez que o S4 esteja corrigido.

### Etapa 3 — Depois do contrato e do banco

1. Acrescentar `button` e `pot` a `AQUARIUM_COMPONENTS` — uma linha, e as duas
   linhas novas aparecem na aba Saúde.
2. Cartão "Controlador" no hub de Diagnóstico.
3. Série de heap nos Relatórios.

---

## Dependências com as outras frentes

- A **Etapa 1 não depende de nada** e pode ser feita em paralelo com o resto.
- As Etapas 2 e 3 são consequência: quando o servidor e o contrato mudarem, o
  front ganha as telas quase de graça, porque a arquitetura já está certa.
- O único item que exige decisão de produto é o cartão "Controlador" — o resto é
  mecânico.
