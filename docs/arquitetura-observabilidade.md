# Arquitetura de observabilidade do BettaCare

Complementa `plano-migracao-bettacare.md`. Aqui ficam as decisões que nasceram
do requisito novo: **saber por que alguma coisa não funcionou**, e não apenas
qual é o estado atual.

Três perguntas concretas guiaram o desenho. Se a implementação não responder as
três, ela não está pronta:

1. *Por que a luminária não acendeu quando eu apertei o botão no app?*
2. *Desde quando o sensor de temperatura sumiu?*
3. *O aquário está saudável agora, componente por componente?*

---

## 1. Ferramental do monorepo

| Camada | Escolha |
|---|---|
| Gerenciador de pacotes | **pnpm** com workspaces |
| Orquestração de tarefas | **Turborepo** (`turbo build`, `turbo dev`) |
| Bundler do Next | **Turbopack** (`next build --turbopack`) |

Vale separar dois nomes que se confundem: **Turborepo** é o orquestrador de
tarefas do monorepo, **Turbopack** é o bundler do Next. São coisas diferentes e
usamos as duas.

### Consequência para o contrato do homelab

`docs/respostas-bettacare.md` está publicado dizendo que o build é
`npm ci && npm run build`. Com pnpm isso muda:

```dockerfile
RUN corepack enable && pnpm install --frozen-lockfile
RUN pnpm turbo build
```

O comando de start (`node dist/server.js`), o uid `1000`, as portas e as env
vars **não mudam**. O agente do servidor precisa ser avisado só do comando de
build — e como ele roda na CI, não no servidor, o impacto na role Ansible é
nenhum. A atualização de `respostas-bettacare.md` entra junto com a Fase 5.

---

## 2. Eventos estruturados

Hoje o firmware guarda 30 strings livres num buffer circular em RAM
(`"[RTC] Automacao: ligando luminaria"`). Não dá para filtrar, agregar nem
alertar em cima disso — é texto.

O evento passa a ser um registro com campos:

```jsonc
{
  "t":    "2026-08-04T17:30:55Z",  // relógio do dispositivo
  "sev":  "error",                 // debug | info | warn | error | fatal
  "comp": "temp",                  // qual componente falou
  "code": "temp.sensor_lost",      // identificador estável, legível por máquina
  "msg":  "DS18B20 nao responde no barramento 1-Wire",
  "ctx":  { "bus_devices": 0 }     // opcional, pequeno
}
```

O `code` é o campo que faz a diferença. É ele que permite a pergunta *"quantas
vezes o DS18B20 caiu este mês?"* sem depender de comparar texto. A `msg` é para
humano e pode mudar de redação sem quebrar nada.

### Componentes (`comp`)

`system`, `wifi`, `api`, `rtc`, `temp`, `fan`, `light`, `button`, `pot`, `nvs`, `ota`

Os cinco primeiros são infraestrutura; os demais são o hardware do aquário.
`button` e `pot` entram porque são entradas físicas que também falham — e um
potenciômetro com mau contato hoje é invisível.

### Origem dupla

A mesma tabela `events` recebe eventos do **firmware** (drenados no POST) e do
**servidor** (comando expirou, dispositivo ficou offline, banco caiu). O campo
`source` (`device` | `server`) distingue. Isso importa: quando o ESP32 está
mudo, quem tem algo a dizer é o servidor, e é justamente aí que o usuário vai
olhar.

### Dedup e volume

O firmware mantém a deduplicação por mensagem repetida que já existe hoje, mas
agora por `(comp, code)` e com contador: em vez de N linhas iguais, uma linha
com `repeat_count`. Um sensor oscilando não pode inundar o banco.

---

## 3. Saúde por componente

Evento é *o que aconteceu*. Saúde é *como está agora*. São tabelas diferentes
porque respondem a perguntas diferentes.

`component_status` tem **uma linha por (dispositivo, componente)**:

| Coluna | Papel |
|---|---|
| `status` | `ok` \| `degraded` \| `missing` \| `fault` \| `unknown` |
| `since` | desde quando está nesse status — é o "sumiu às 14:02" |
| `last_ok_at` | último instante em que esteve `ok` |
| `detail` | JSON pequeno com o valor corrente (rpm, °C, rssi) |
| `last_code` | o `code` do evento que causou a transição atual |

Só grava quando o status **muda**. É uma tabela minúscula que nunca cresce, e é
ela que alimenta a aba Saúde direto, sem varrer histórico.

### Como cada componente é julgado

| Componente | `missing` | `degraded` | `fault` |
|---|---|---|---|
| `temp` (DS18B20) | nenhum device no barramento 1-Wire | leitura *stale* > 30 s | CRC inválido repetido |
| `rtc` (DS3231) | I²C não responde | `lostPower` (bateria) | hora fora de faixa plausível |
| `fan` | — | pot fora da faixa de ADC esperada | **duty > 0 e tacômetro em 0 por > 5 s** |
| `light` (SSR) | — | — | **estado desejado ≠ estado reportado após 2 ciclos** |
| `wifi` | — | RSSI < -80 dBm | N reconexões em janela curta |
| `button` | — | preso pressionado > 30 s | — |
| `api` | — | POST falhou 1–3× seguidas | POST falhando > 60 s |

Duas linhas dessa tabela merecem atenção porque são as que respondem à pergunta
*"por que não consigo controlar?"*:

**Ventoinha em `fault`** — o tacômetro é o único componente com realimentação
real. Duty acima de zero e RPM zerado significa fisicamente uma coisa só: a
ventoinha não está girando. Cabo solto, rolamento travado ou fonte caída. Hoje
isso é invisível.

**Luminária em `fault`** — o SSR não tem retorno, então a única evidência
possível é a divergência entre o que foi comandado e o que o firmware reporta.
Se o servidor mandou `light.set{on:true}`, o dispositivo confirmou o comando, e
dois POSTs depois `light.on` continua `false`, alguma coisa está errada entre o
GPIO e a lâmpada. É um diagnóstico honesto: não afirma *qual* é o defeito, mas
afirma com segurança que existe um.

---

## 4. Ciclo de vida do comando

Esta é a resposta direta a *"qual erro não está me permitindo controlar a
luminária"*. Hoje um comando some no éter e não há onde olhar.

```
queued ──► sent ──► acked        (deu certo)
   │        │
   │        ├────► rejected      (dispositivo recusou, com código)
   │        └────► expired       (sem ack dentro do TTL)
   └─────────────► superseded    (comando mais novo para o mesmo alvo)
```

### O `ack` deixa de ser uma lista de números

O contrato publicado prevê `"ack": [91]`. Isso só sabe dizer *"recebi"*, nunca
*"não consegui"*. Passa a ser:

```jsonc
"ack": [
  { "id": 91, "ok": true },
  { "id": 92, "ok": false, "code": "light.gpio_fault" }
]
```

O custo no ESP32 é de alguns bytes por comando confirmado, e comandos são
raros. O ganho é que a falha volta com nome.

### Quem decide `expired`

O watchdog de 30 s que já estava previsto. Se um comando está `sent` há mais de
`COMMAND_TTL_S` (padrão 90 s) sem `ack`, vira `expired` e gera um evento de
servidor com `code: cmd.expired`. Se o dispositivo nunca chegou a receber
(continua `queued` e não há POST há mais de 15 s), o evento é
`cmd.device_offline` — são causas diferentes e o usuário precisa distinguir.

### O que a interface mostra

Ao tocar em "Ligar luminária", o botão não volta a verde sozinho. Ele passa por
*enviando → confirmado*, ou por *enviando → falhou*, e nesse caso exibe a razão
em português direto:

- "Dispositivo offline desde 14:02 — comando aguardando na fila"
- "O dispositivo recusou: falha no GPIO da luminária"
- "Comando expirou sem confirmação"
- "Servidor sem banco de dados"

Cada uma dessas frases vem de um `code` distinto, não de heurística no front.

---

## 5. Retenção: o teto de 1 GB

Requisito: o banco `bettacare` nunca passa de **1 GB**; ao chegar lá, as
entradas mais antigas saem.

### O detalhe que quebra a implementação ingênua

`DELETE` no PostgreSQL **não devolve espaço ao sistema de arquivos**. As linhas
viram tuplas mortas, o `VACUUM` comum apenas marca as páginas como reutilizáveis,
e `pg_database_size()` continua exatamente igual. Um job que apaga linhas antigas
e mede o tamanho depois entraria em laço infinito: apaga, mede, nada mudou,
apaga de novo — até esvaziar a tabela inteira.

`VACUUM FULL` devolveria o espaço, mas pega `ACCESS EXCLUSIVE` na tabela e
precisa de espaço livre equivalente ao dobro dela. Num Postgres compartilhado
com outros serviços do homelab, é inaceitável.

**O que devolve espaço de imediato é `DROP TABLE`.** Por isso `telemetry` e
`events` são **tabelas particionadas por mês** (`PARTITION BY RANGE (received_at)`),
e a purga derruba a partição mais antiga inteira. Instantânea, sem lock na
tabela pai, sem inchaço.

```
telemetry_2026_08   telemetry_2026_09   telemetry_2026_10  ← partições mensais
                 └── DROP quando o banco passa do teto
```

### O job

Roda uma vez por dia, junto com o rollup (03:15):

1. Garante que a partição do mês corrente e a do mês seguinte existem
   (criar adiantado evita erro de insert na virada do mês).
2. Lê `pg_database_size(current_database())`.
3. Enquanto o tamanho > `DB_SIZE_LIMIT_BYTES` (padrão `1073741824`), derruba a
   partição mais antiga entre `telemetry` e `events` — a que for maior primeiro.
4. Nunca derruba a partição do mês corrente, mesmo se sozinha estourar o teto.
   Nesse caso registra um evento `db.size_limit_unreachable` e para. Um teto
   inatingível é um problema para o humano resolver, não para o job insistir.

`telemetry_hourly`, `settings`, `commands`, `devices` e `component_status` são
**isentos**. Somados dão poucos megabytes e são justamente o que dá valor ao
histórico longo: a curva de temperatura do ano passado sobrevive mesmo depois
de a telemetria bruta daquele mês ter sido descartada.

### Uma consequência do particionamento

O PostgreSQL exige que toda constraint `UNIQUE` de uma tabela particionada
contenha a chave de partição. Ou seja, o `UNIQUE (device_id, boot_id, seq)`
prometido em `respostas-bettacare.md` **não é possível** como está — teria de
virar `UNIQUE (received_at, device_id, boot_id, seq)`, o que enfraquece a
garantia: o mesmo `seq` reenviado poderia entrar duas vezes se as duas
tentativas caíssem em meses diferentes.

A idempotência migra para onde ela é mais barata: `device_state` guarda
`last_seq` e `last_boot_id`, e o ingest descarta o POST cujo `seq` não avança.
É uma checagem de uma linha, com índice pela PK, e cobre o caso do reenvio de
verdade — que acontece em segundos, não em meses.

### Volume esperado

Com gravação **on-change + heartbeat de 60 s** (a decisão tomada), são cerca de
2.000 linhas/dia, ~150 MB/ano com índices. O teto de 1 GB só seria alcançado por
volta do sexto ano — na prática ele é uma **rede de segurança** contra um
componente defeituoso que comece a gerar eventos em rajada, não o regime normal
de operação. Ainda assim precisa funcionar, porque o cenário que ele cobre é
exatamente aquele em que ninguém está olhando.

---

## 6. Atualização forçada do PWA

O problema clássico: o service worker serve a versão em cache, o usuário fica
semanas numa build velha e nem sabe.

### Cabeçalhos, que é onde a briga é ganha

Como o Hono serve o export estático, os cabeçalhos são nossos — não dependemos
de configuração de CDN:

| Recurso | `Cache-Control` |
|---|---|
| `/_next/static/**` (nome com hash) | `public, max-age=31536000, immutable` |
| `/sw.js`, `/version.json`, `*.html`, `/` | `no-store` |

O `sw.js` com `no-store` é o ponto crítico: um service worker em cache é um
service worker que nunca se atualiza.

### O carimbo de versão

A build gera `/version.json`, que **não** entra no precache:

```json
{ "buildId": "a3f9c21", "version": "1.4.0", "minBuild": "a1b2c33", "builtAt": "..." }
```

O nome do cache do service worker inclui o `buildId`. Ativação nova apaga todos
os caches de buildId diferente e chama `clients.claim()`.

### O ciclo

O app consulta `/version.json` na abertura, a cada retorno de foco
(`visibilitychange`) e a cada 15 minutos. Se o `buildId` diverge:

- **Caso normal** — mostra uma faixa "Nova versão disponível · Atualizar". O
  toque manda `SKIP_WAITING` ao worker em espera e recarrega no
  `controllerchange`. Só recarrega uma vez, com trava, senão vira laço.
- **Caso forçado** — se o `buildId` corrente for **anterior ao `minBuild`**
  declarado pela build nova, recarrega sozinho, sem perguntar. É a válvula para
  quando uma versão quebrada foi ao ar ou o contrato da API mudou de forma
  incompatível.

Optei por **service worker escrito à mão** (~120 linhas) em vez de Serwist ou
next-pwa. O app tem quatro telas e o requisito aqui é controle exato do momento
do `skipWaiting` — que é justamente a parte que as bibliotecas abstraem. A
dependência custaria mais em integração com `output: 'export'` do que economiza
em código.

---

## 7. A interface

Quatro abas na barra inferior, como hoje. Diagnóstico é a quarta e concentra
tudo que é "olhar para trás":

```
Início · Luminária · Ventoinha · Diagnóstico
                                    └── Saúde · Logs · Relatórios
```

**Saúde** — lista dos componentes com pill de estado, valor corrente e "desde
HH:MM". Tocar abre o detalhe: histórico de transições daquele componente e os
eventos que ele gerou.

**Logs** — chips de severidade (Tudo / Erros / Avisos / Info), seletor de
componente e busca por texto. Lista paginada por cursor, horário convertido para
`TZ_DISPLAY`. Origem `device` e `server` marcadas visualmente.

**Relatórios** — curva de temperatura, horas de luz por dia, tempo de ventoinha
ligada. Alimentado por `telemetry_hourly`, então continua funcionando em cima de
períodos cuja telemetria bruta já foi purgada.

O desenho visual é o do app antigo, portado: paleta escura (`#070d1a` de fundo,
`#4cc3f7` de acento, âmbar para luz, verde-água para ventoinha), cards com borda
de 1px, pills de estado, hero de temperatura. Reescrito em React, não
reinventado.

---

## 8. Ordem de execução

| Fase | Entregável |
|---|---|
| 0 | Esqueleto pnpm + Turborepo, TypeScript, lint |
| 1 | `packages/contract` em Zod + schema Drizzle + migrations rodando em Postgres local |
| 2 | Servidor Hono: duas portas, `/healthz`, ingest idempotente, fila de comandos com ack detalhado, watchdog, job de retenção |
| 3 | Firmware: task FreeRTOS de rede, eventos estruturados, `boot_id`/`seq`, ack com erro |
| 4 | PWA: quatro telas, hub de diagnóstico, service worker com atualização forçada |
| 5 | CI, imagem em `ghcr.io`, atualização de `respostas-bettacare.md` |

A Fase 2 termina antes de encostar no firmware: assim o ESP32 é desenvolvido
contra um servidor que já funciona e pode ser simulado inteiro com `curl`.
