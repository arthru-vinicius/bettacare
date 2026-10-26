# A interface (PWA)

Next.js 16 com `output: 'export'`, servido pelo Hono no mesmo processo do
servidor. Não há Next.js server, não há proxy, não há segundo container.

Uma página só, com as telas trocadas por estado do cliente. Com export
estático isso também simplifica o service worker: há um único HTML a manter
em cache e nenhuma navegação a interceptar.

```
Início · Luminária · Ventoinha · Alimentador · Diagnóstico
                                                 └── Saúde · Registros
```

---

## Telas

### Início

Tudo o que tem tela própria é tocável: a temperatura e o cartão da ventoinha
levam à Ventoinha; o cartão da luminária, à Luminária.

### Luminária

- Os horários "Acende" e "Apaga" abrem o editor de horário (`PUT /api/settings`).
- **Tempo aceso**: por hora nas últimas 24 h e por dia em 7 e 30 dias, com o
  horário programado tracejado como referência. Vem de `telemetry_hourly`; a hora
  em curso só entra depois de fechar.

### Ventoinha

- Status primeiro (na mesma ordem da luminária), depois a temperatura, que abre o
  editor dos limites. Um botão Ligar/Desligar que troca de texto, e "Voltar ao
  automático" habilitado só no manual.
- **Histórico da temperatura.** Um seletor de período (24 h, 7 ou 30 dias)
  governa dois cartões. O primeiro é uma curva em que **tocar, arrastar (ou passar
  o mouse) mostra a temperatura exata e o dia e a hora** daquele momento — acima
  do gráfico, onde o dedo não cobre. O toque fixa o ponto ("Limpar" ou Esc
  solta); pelo teclado, as setas andam ponto a ponto. A rolagem vertical da
  página continua valendo por cima do gráfico; só o arraste na horizontal é
  dele. Mostra os limites de liga e desliga da ventoinha (só quando a água está
  perto deles), a faixa de quando ela esteve ligada, os silêncios do aquário
  como buracos (ligar os dois lados por uma reta seria desenhar uma temperatura
  que ninguém mediu) e mínima/média/máxima do período — mínima e máxima levam o
  cursor ao momento em que aconteceram. Há também uma tabela com a mesma série,
  para quem não usa o gráfico.
- A resolução é de 1 min em 24 h, e a série vem da telemetria bruta e da tabela
  `fan_runs`, não do rollup horário (ver `GET /api/history/fan` em
  [`api.md`](./api.md)). A faixa habitual da água (16–33 °C) não esconde leitura:
  o que se plota é tudo o que é plausível (10–45 °C).
- **Acionamentos.** O segundo cartão, com o mesmo período do gráfico: quantos
  foram, quanto tempo a ventoinha ficou ligada, duração e variação média de
  temperatura, e a tabela — quando ligou, quando desligou, quanto girou, de que
  temperatura partiu e onde chegou (com ▼ esfriou, ▲ esquentou ou ≈ estável, e o
  pico). Tocar numa linha a mostra no gráfico. A rodada em curso conta o tempo ao
  vivo e mostra a temperatura de agora. Sem nenhum acionamento, o cartão diz por
  quê: "a água ficou entre 21,9 e 28,8 °C, sempre abaixo dos 29,0 °C em que a
  ventoinha liga".
- O seletor de período fica **uma vez, acima dos cartões** que ele governa, e
  trocar de período não pisca: os cartões antigos esmaecem até os novos chegarem.

### Alimentador

Mostra "N de 3 refeições nas últimas 24 h", em âmbar com "limite atingido"
quando chega a 3. Nesse caso, "Alimentar agora" não manda nada direto: abre a
folha "Limite de refeições atingido", que explica o risco e oferece "Alimentar
mesmo assim" (`feeder.feed_now` com `force: true`). É o único caminho para
passar do limite — pelo botão do módulo, não há.

### Diagnóstico

Concentra tudo o que é "olhar para trás".

- **Saúde.** Lista dos componentes com pill de estado, valor corrente e "desde
  HH:MM". Tocar abre o detalhe: histórico de transições daquele componente e os
  eventos que ele gerou. Tem também o ajuste "Atualização do aquário" (1, 2, 3
  ou 5 s), que grava `telemetry_interval_ms` na configuração.
- **Registros.** Período (hoje, 7 dias, 30 dias ou datas), nível, filtros extras
  recolhidos (componente, texto, origem), lista agrupada por dia com detalhes ao
  toque, horário em `TZ_DISPLAY` e origem `device` ou `server` marcada. Daqui
  saem as planilhas (eventos e medições, CSV para o Excel em português), com os
  mesmos filtros.

## Aparência

Paleta escura — `#070d1a` de fundo, `#4cc3f7` de acento, âmbar para a
luminária, verde-água para a ventoinha —, cards com borda de 1px, pills de
estado e hero de temperatura. CSS puro, sem Tailwind: são poucas telas, e a
folha inteira cabe em menos que a configuração da alternativa.

A linha da temperatura é um azul próprio (`--c-serie-temp`), não o ciano de
destaque: ciano e verde-água da ventoinha ficavam a ΔE 12 (abaixo dos 15 que a
visão normal precisa para distinguir duas séries); o azul fica a ΔE 19.

Os editores são folhas que sobem do rodapé (`components/Sheet.tsx`),
renderizadas por portal no `body`: as telas animam com `transform`, e dentro
delas um `position: fixed` ficaria preso, com os botões atrás da barra de
navegação. Campos de hora e data são os seletores nativos do sistema.

---

## Ícones e tela de abertura

Duas artes, dois papéis (`apps/web/assets/`): o **lançador** (ilustração de
contorno limpo) é o ícone pequeno — gaveta de apps, tela inicial, canto do app;
a **arte de carregamento** (fotorrealista) só aparece grande — na abertura do
Android e na tela de carregamento do próprio app (`Carregando.tsx`).

A abertura do Android é o detalhe que engana: o manifest não tem campo de
splash, e o Chrome a monta com `background_color` e um dos `icons`, preferindo
os `maskable` — o mesmo grupo de onde sai o ícone da gaveta. Até a v1.4.0 o
único maskable era o do lançador, que aparecia grande e cortado na abertura.
Agora são dois, separados pelo tamanho que cada uso procura (83dp na gaveta,
128dp na abertura): o lançador em 332 px e a arte de carregamento em 512 px. A
gaveta fica sempre com o lançador; a abertura mostra a arte de carregamento em
telas de densidade 2,625 ou mais (o padrão dos celulares atuais) e, abaixo
disso, o lançador, como antes. A conta está em `scripts/gerar-icones.py`.

O Android só atualiza os ícones de um app já instalado quando o Chrome revisa
o manifest (no máximo uma vez por dia, ao abrir o app); para ver na hora,
desinstale e instale o app de novo.

---

## Os tipos vêm do contrato

`packages/contract/src/api.ts` define também a superfície do navegador, não só a
do ESP32. O servidor e a interface tipam pelo mesmo módulo.

A razão é evitar divergência: com o formato do JSON escrito à mão em três
lugares (firmware, servidor e interface), os três acabam divergindo.

---

## Atualização forçada

O problema clássico: o service worker serve a versão em cache, e o usuário fica
semanas numa build velha sem saber. A defesa tem três camadas, e as três
precisam existir.

**1. Cabeçalhos.** Definidos pelo Hono em `apps/server/src/static.ts` (tabela em
[`api.md`](./api.md#cabeçalhos-de-cache)): `sw.js`, `version.json`, o HTML e o
manifest são `no-store`; `/_next/static/**` é `immutable`. O `sw.js` é o crítico
— um service worker em cache é um service worker que nunca se atualiza.

**2. Carimbo de versão.** `scripts/stamp-version.mjs` roda depois do
`next build` e faz duas coisas: escreve `out/version.json` e substitui
`__BUILD_ID__` dentro de `out/sw.js`. O `version.json` **não** entra no
precache:

```json
{ "buildId": "a3f9c21", "version": "1.4.0", "minBuild": "a1b2c33", "builtAt": "..." }
```

O `buildId` nomeia o cache do service worker, então build nova invalida tudo
sozinha: a ativação apaga os caches de `buildId` diferente e chama
`clients.claim()`. O script **falha alto** se o marcador não for encontrado: um
`sw.js` sem carimbo prenderia o app na primeira build instalada para sempre.

**3. O ciclo.** O app consulta `/version.json` na abertura, a cada retorno de
foco (`visibilitychange`) e a cada 15 minutos.

| Situação | O que acontece |
|---|---|
| `buildId` diverge | faixa "Nova versão disponível · Atualizar" |
| `buildId` corrente **anterior** ao `minBuild` | recarrega sozinho, sem perguntar |

O toque em "Atualizar" manda `SKIP_WAITING` ao worker em espera e recarrega no
`controllerchange`, uma vez só e com trava — senão vira laço. O `minBuild` fica
nulo no caminho normal; é a válvula para quando uma versão quebrada foi ao ar ou
o contrato da API mudou de forma incompatível: define-se `MIN_BUILD` no ambiente
da CI e a frota inteira recarrega.

O service worker **não chama `skipWaiting` na instalação**. Trocar por baixo dos
pés de quem está no meio de uma ação é como se perde estado; ele fica em espera
até o toque do usuário, ou até a recarga forçada.

O service worker é escrito à mão (~200 linhas), em vez de Serwist ou next-pwa. O
requisito aqui é controle exato do momento do `skipWaiting` — justamente a parte
que as bibliotecas abstraem —, e a dependência custaria mais em integração com
`output: 'export'` do que economiza em código.

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
| `pending` | "Enviando ao aquário… Ele busca os comandos a cada 1 segundo." (o intervalo configurado) |
| na fila (aquário offline) | "Na fila — o aquário está sem contato desde 14:02" + "fica guardado até 14:17 e é executado assim que ele voltar" (âmbar, não vermelho) |
| `acked` | "Confirmado pelo aquário", ou "Executado pelo aquário quando ele voltou" se passou pela fila |
| `rejected` | o rótulo do catálogo, ex.: "Falha ao acionar a luminária" + a dica |
| `expired`, `cmd.stale_discarded` | "Não foi executado — o aquário ficou sem contato o prazo todo" |
| `expired`, `cmd.expired` | "Sem confirmação do aquário" — pode ter executado; conferir o estado |
| `expired`, `cmd.interrupted` | o rótulo do catálogo: o controlador reiniciou no meio (alimentação não é repetida) |

Cada frase vem de um estado distinto, não de heurística: o `error_code` do
servidor separa "nunca chegou" de "chegou e a confirmação não voltou" e de
"reiniciou no meio", e o usuário precisa distinguir para saber se repete o
comando ou se confere o que aconteceu.

**Na fila não é falha** (v1.3.0). Com o Wi-Fi do aquário caindo por alguns
minutos várias vezes ao dia, o comando fica no servidor por 15 min e sai na
volta. Até a v1.2.0 isso aparecia em vermelho e o app parava de acompanhar —
o aquário executava depois e a tela nunca sabia. Agora o app segue observando
(no ritmo normal de 2 s, não no rápido: sem contato, pressa não adianta) e
mostra o desfecho real.

O feedback é **associado ao alvo do comando**. Sem isso ele é global e vaza —
uma falha da luminária apareceria também na tela da ventoinha.

Mudança de configuração tem o próprio aviso (`SettingsStatus`), com a
distinção que importa: *salvo* (o servidor gravou) não é *aplicado* (o aquário
buscou a versão nova e diz estar usando). Sem contato com o aquário, fica
"salvo no servidor — aplica quando voltar".

---

## Polling

Polling e não WebSocket: são três usuários e acessos esparsos, o dado só muda
no ritmo da telemetria, e uma conexão persistente custaria complexidade de
reconexão para ganhar pouco.

- 2 s com a aba visível; 0,6 s enquanto há comando ou configuração aguardando
  desfecho. Com o aquário a 1 s e a confirmação enviada logo depois da
  execução (firmware), do toque ao "confirmado" leva cerca de 1–2 s.
- **Para quando a aba fica oculta.** Sem isso o app deixado aberto no celular
  faria requisições indefinidamente.

---

## Verificação

### Verificado

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

Com o servidor local, Postgres 17 e um aquário simulado obedecendo aos
comandos como o firmware:

- ligar e desligar a ventoinha confirmados em menos de 1 s; "Voltar ao
  automático" desabilitado no automático e habilitado no manual;
- horário da luminária editado, salvo, aplicado pelo aquário e registrado
  ("acende 09:30 e apaga 18:15 (era 10:00–17:00)");
- limites da ventoinha: o editor recusa desligamento colado no acionamento,
  e a gravação válida é aplicada e registrada;
- planilhas de registros e de medições baixadas pelos links da tela, com os
  filtros aplicados;
- atualização a 1 s escolhida na Saúde: aplicada, 10 POSTs em 10 s, nenhum
  alarme falso.

Em produção:

- atrás do Cloudflare Access: é como o app é usado, e a mudança de
  configuração ficou registrada com o e-mail do cabeçalho
  `Cf-Access-Authenticated-User-Email`.

### Não verificado

- instalação como PWA no iOS e no Android (a heurística de instalabilidade
  depende do navegador real, não do headless);
- a tela em aparelhos estreitos de verdade; foi conferida em 390×844.
