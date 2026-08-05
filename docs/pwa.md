# A interface (PWA)

Referência da Fase 4.

---

## Estrutura

Next.js 16 com `output: 'export'`, servido pelo Hono no mesmo processo do
servidor. Não há Next.js server, não há proxy, não há segundo container.

Uma página só, com as telas trocadas por estado do cliente — como no app
antigo. Com export estático isso também simplifica o service worker: há um
único HTML a manter em cache e nenhuma navegação a interceptar.

```
Início · Luminária · Ventoinha · Diagnóstico
                                    └── Saúde · Logs · Relatórios
```

O card de temperatura do Início é tocável e leva aos Relatórios — foi a escolha
para manter quatro abas em vez de cinco, sem esconder o histórico.

O desenho é o do app antigo, portado: `#070d1a` de fundo, `#4cc3f7` de acento,
âmbar para a luminária, verde-água para a ventoinha, cards com borda de 1px,
pills de estado, hero de temperatura. CSS puro, sem Tailwind: são quatro telas,
e a folha inteira cabe em menos que a configuração da alternativa.

---

## Os tipos vêm do contrato

`packages/contract/src/api.ts` define também a superfície do navegador, não só a
do ESP32. O servidor e a interface tipam pelo mesmo módulo.

É a mesma razão de sempre: no sistema antigo o formato do JSON estava escrito à
mão em três lugares e os três já divergiam entre si.

---

## Atualização forçada

A defesa tem três camadas, e as três precisam existir.

**1. Cabeçalhos.** Definidos pelo Hono em `apps/server/src/static.ts`:
`sw.js`, `version.json`, o HTML e o manifest são `no-store`; `/_next/static/**`
é `immutable`. O `sw.js` é o crítico — um service worker em cache é um service
worker que nunca se atualiza.

**2. Carimbo de versão.** `scripts/stamp-version.mjs` roda depois do
`next build` e faz duas coisas: escreve `out/version.json` com o `buildId`, e
substitui `__BUILD_ID__` dentro de `out/sw.js`. O `buildId` nomeia o cache, então
build nova invalida tudo sozinha. O script **falha alto** se o marcador não for
encontrado: um `sw.js` sem carimbo prenderia o app na primeira build instalada
para sempre.

**3. O ciclo.** O app consulta `/version.json` na abertura, a cada retorno de
foco e a cada 15 minutos.

| Situação | O que acontece |
|---|---|
| `buildId` diverge | faixa "Nova versão disponível · Atualizar" |
| `buildId` corrente **anterior** ao `minBuild` | recarrega sozinho, sem perguntar |

O `minBuild` fica nulo no caminho normal. É a válvula para quando uma versão
quebrada foi ao ar ou o contrato da API mudou de forma incompatível — define-se
`MIN_BUILD` no ambiente da CI e a frota inteira recarrega.

O service worker **não chama `skipWaiting` na instalação**. Trocar por baixo dos
pés de quem está no meio de uma ação é como se perde estado; ele fica em espera
até o toque do usuário, ou até a recarga forçada.

### Estratégias de cache

| Recurso | Estratégia | Por quê |
|---|---|---|
| `/_next/static/**` | cache primeiro | tem hash no nome, nunca muda de conteúdo |
| navegação | rede primeiro, cache de reserva | cache primeiro prenderia numa build velha |
| `/api/*`, `/version.json` | nunca cacheado | estado corrente e o próprio detector de versão |
| ícones, manifest | cache com revalidação | mudam raro, e o atraso de um ciclo não custa nada |

---

## Feedback de comando

O botão não volta ao normal sozinho. Ele passa por *enviando → confirmado*, ou
*enviando → falhou* com a razão:

| Estado | O que o usuário lê |
|---|---|
| `pending` | "Enviando ao dispositivo… Ele busca a fila a cada 3 segundos." |
| `acked` | "Confirmado pelo dispositivo" |
| `rejected` | o rótulo do catálogo, ex.: "Falha ao acionar a luminária" + a dica |
| `expired`, offline | "Dispositivo sem contato desde 14:02" + o que esperar |
| `expired`, online | "O comando expirou sem confirmação" + a causa provável |

Cada frase vem de um estado distinto, não de heurística: "expirou" com o
dispositivo offline é uma causa diferente de "expirou" com ele online, e o
usuário precisa distinguir para saber se o problema é a rede ou o aquário.

O feedback é **associado ao alvo do comando**. Sem isso ele é global e vaza —
uma falha da luminária apareceria também na tela da ventoinha.

---

## Polling

Polling e não WebSocket: são três usuários e acessos esparsos, o dado só muda a
cada 3 s de qualquer forma, e uma conexão persistente custaria complexidade de
reconexão para ganhar nada perceptível.

- 4 s com a aba visível, 1,5 s enquanto há comando aguardando desfecho.
- **Para quando a aba fica oculta.** Sem isso o app deixado aberto no celular
  faria uma requisição a cada 4 s indefinidamente.

---

## Verificado no navegador

Com o servidor real, dados semeados e Postgres 17:

- as quatro telas renderizam, zero erro ou aviso no console;
- ventoinha travada aparece em vermelho na aba Saúde com a causa provável, RTC
  sem bateria e Wi-Fi fraco em âmbar;
- filtros de log por severidade, equipamento e origem;
- relatório de 7 dias confere com a semente: 49 h de luz (7 h/dia) e 14 h de
  ventoinha;
- comando recusado pelo dispositivo mostra "Falha ao acionar a luminária" com a
  dica do SSR — não o código cru;
- service worker registrado, ativo, controlando a página, cache nomeado
  `bettacare-<buildId>`;
- **atualização normal**: faixa apareceu ao publicar um `buildId` diferente;
- **atualização forçada**: com `minBuild` acima do build do cliente, a página
  recarregou sozinha, sem faixa e sem interação.

## Não verificado

- instalação como PWA no iOS e no Android (a heurística de instalabilidade
  depende do navegador real, não do headless);
- comportamento atrás do Cloudflare Access — em desenvolvimento não há o header
  `Cf-Access-Authenticated-User-Email`, e o servidor aceita a ausência;
- a tela em aparelhos estreitos de verdade; foi conferida em 390×844.
