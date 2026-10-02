# Servidor e banco — achados e plano de ação

Auditoria de `apps/server/src/**` e das migrations em `apps/server/drizzle/`.

**Veredito geral:** o servidor está num nível acima do firmware em maturidade. A
idempotência por `last_seq` está correta, a comparação do token em tempo
constante é exemplar, o rollup ponderado por duração é a modelagem certa, e o
particionamento por mês com purga por `DROP` resolve de verdade o problema que
`DELETE` não resolve.

Os problemas são de **cobertura**, não de corretude: o schema não guarda os
dados que diagnosticariam falha intermitente, e três regras prometidas em
`docs/arquitetura-observabilidade.md` não têm implementação.

---

## Sumário dos achados

| # | Achado | Onde | Gravidade |
|---|---|---|---|
| S1 | Schema sem nenhum dado de diagnóstico do ESP32 | `db/schema.ts:97` | **Alta** (lacuna) |
| S2 | `button` e `pot` nunca são avaliados — saúde inexistente | `health/evaluate.ts:56` | **Alta** |
| S3 | Saúde de `api` é hardcoded `ok` — a regra do documento não existe | `health/evaluate.ts:64` | **Alta** |
| S4 | Comandos em `queued` nunca expiram; `cmd.device_offline` não existe | `jobs/watchdog.ts:69` | **Alta** |
| S5 | Rollup só cobre "ontem" — dias perdidos somem para sempre | `jobs/maintenance.ts:128` | **Alta** |
| S6 | `deviceId: "aquarium-01"` hardcoded em dois lugares | `jobs/maintenance.ts:53,74` | **Média** |
| S7 | `events.device_time` é sempre nulo — o firmware manda em outro lugar | `ingest/process.ts:326` | **Média** |
| S8 | `ack` para comando já expirado é descartado em silêncio | `ingest/process.ts:203` | **Média** |
| S9 | Histórico não guarda RTC, reconexões nem idade da leitura | `db/schema.ts:202` | **Média** |
| S10 | Primeira transição de saúde de um componente não vira evento | `ingest/process.ts:290` | **Média** |
| S11 | Sem limite de tamanho de corpo no ingest | `ingest/router.ts:54` | **Média** |
| S12 | `fan.tach_stalled` com falso positivo estrutural | `health/evaluate.ts:112` | **Média** |
| S13 | ~16 consultas por POST; saúde faz 11 idas ao banco | `ingest/process.ts:263` | **Baixa** |
| S14 | `lastRunDay` em memória — manutenção pode rodar duas vezes | `jobs/maintenance.ts:23` | **Baixa** |

---

## Os achados em detalhe

### S1 — O banco não guarda nada que diagnostique falha intermitente

**Alta**, e é o centro do seu pedido: *"quero um banco que consiga armazenar
informações ricas de saúde do ESP"*.

O que `device_state` guarda hoje (`db/schema.ts:97-162`) é o **estado do
aquário**: luz, temperatura, ventoinha, RTC, RSSI, IP, reconexões, uptime. É bom
e está correto. O que ele não guarda é o **estado do controlador**.

Nenhuma destas perguntas tem resposta no banco atual:

- O ESP32 reiniciou esta noite? Por quê?
- A memória livre está caindo ao longo dos dias?
- O POST está demorando mais do que costumava?
- O potenciômetro está com mau contato?
- Quantas vezes seguidas o dispositivo falhou em falar com o servidor?

A causa é que os dados não chegam — ver F7 no [plano de firmware](03-plano-firmware.md).
Mas a correção precisa das duas pontas, e a do banco vem primeiro: campo novo no
firmware sem coluna correspondente é rejeitado pelo contrato no ingest.

**DDL proposta.** Duas frentes: estado corrente e série temporal.

```sql
-- Estado corrente: sempre o último valor conhecido.
alter table device_state
  add column reset_reason        text,
  add column free_heap           integer,
  add column min_free_heap       integer,
  add column max_alloc_heap      integer,
  add column post_latency_ms     integer,
  add column api_failures        integer      not null default 0,
  add column pot_raw_adc         integer,
  add column button_pressed      boolean      not null default false,
  add column tach_pulses_raw     integer,
  add column net_task_stack_hwm  integer,
  add column boot_count          integer;

-- Série temporal: só o que faz sentido plotar ao longo do tempo.
alter table telemetry
  add column free_heap        integer,
  add column max_alloc_heap   integer,
  add column post_latency_ms  integer,
  add column pot_raw_adc      integer,
  add column temp_age_ms      bigint,
  add column rtc_available    boolean,
  add column rtc_lost_power   boolean,
  add column wifi_reconnects  integer;
```

Notas sobre a modelagem, porque duas escolhas aqui não são óbvias:

**`reset_reason` fica só em `device_state`, não em `telemetry`.** Ele muda uma
vez por boot; guardá-lo em cada linha de histórico seria repetir a mesma string
duas mil vezes por dia. O evento de boot é o lugar certo do histórico — o
firmware já emite `system.boot` (`api_client.cpp:55`), e basta acrescentar o
motivo ao `ctx`. Aí ele fica pesquisável em `events` sem custo de coluna.

**`min_free_heap` e `max_alloc_heap` valem mais que `free_heap`.** O heap livre
instantâneo, amostrado a cada 3 s, quase nunca pega o pior momento. O mínimo
histórico pega. E a diferença entre "muito heap livre" e "maior bloco alocável
pequeno" é exatamente o diagnóstico de fragmentação — que é a hipótese do F8 do
firmware. Sem `max_alloc_heap` não dá para confirmar nem descartar.

**Sobre o `uptime_ms`.** A coluna já é `bigint`, e o comentário em
`db/schema.ts:134` reconhece o estouro do `integer`. Falta reconhecer o outro
estouro: `millis()` volta a zero em **49,7 dias**. Um aquário fica ligado meses,
então isso vai acontecer. Qualquer regra futura que infira reboot a partir de
queda no uptime estará errada — **use `boot_id`**, que é o campo projetado para
isso. Vale um comentário no schema antes que alguém escreva essa regra.

### S2 — `button` e `pot` não têm avaliação nenhuma

**Alta.** `evaluateHealth` (`health/evaluate.ts:56-65`) devolve exatamente seis
vereditos: `temp`, `rtc`, `fan`, `light`, `wifi` e `api`.

A tabela da seção 3 de `docs/arquitetura-observabilidade.md` lista `button` com
regra própria ("preso pressionado > 30 s") e `pot` ("fora da faixa de ADC
esperada"). Nenhum dos dois é avaliado. E o texto do documento é explícito sobre
por que eles entraram na lista:

> `button` e `pot` entram porque são entradas físicas que também falham — e um
> potenciômetro com mau contato hoje é invisível.

Ele continua invisível. Do lado do firmware, `COMP_POT` existe mas só é usado
para relatar mudança de velocidade, e não há nenhum campo de botão ou de ADC no
payload.

**Correção:** depende do firmware enviar `button_pressed` e `pot_raw_adc` (F7 e
F10 do plano de firmware). Com eles no payload, as duas regras são pequenas:

```ts
function evaluateButton(t: TelemetryRequest): HealthVerdict {
  // Preso: o firmware já sabe disso e emite button.stuck; aqui basta refletir.
  if (t.diag.button_pressed_ms > 30_000) {
    return { comp: "button", status: "degraded", code: "button.stuck", detail };
  }
  return { comp: "button", status: "ok", code: null, detail };
}

function evaluatePot(t: TelemetryRequest): HealthVerdict {
  const adc = t.diag.pot_raw_adc;
  // Fora de 0..4095 é impossível; colado no extremo é mau contato ou saturação.
  if (adc === null) return { comp: "pot", status: "unknown", code: null, detail };
  if (adc >= 4090) {
    return { comp: "pot", status: "degraded", code: "pot.saturated", detail };
  }
  return { comp: "pot", status: "ok", code: null, detail };
}
```

O limite superior conversa com a nota sobre atenuação do ADC no
[documento 02](02-hardware-e-ligacoes.md), §8 — confirme o comportamento real na
bancada antes de fixar o número.

### S3 — A saúde de `api` é uma tautologia

**Alta.** `health/evaluate.ts:62-65`:

```ts
// Se este POST chegou, a comunicação está funcionando. É tautológico, mas
// é o que faz a aba Saúde mostrar a linha verde em vez de "sem informação".
{ comp: "api", status: "ok", code: null, detail: null },
```

O comentário é honesto, e a decisão é defensável como recurso provisório. Mas o
documento promete outra coisa: `api` fica `degraded` com "POST falhou 1–3×
seguidas" e `fault` com "POST falhando > 60 s".

O dado existe. `api_client.cpp:20` mantém `_failures`, e
`api_client_consecutive_failures()` é público (`api_client.cpp:59`) — mas nunca
é serializado. O dispositivo sabe que tentou seis vezes antes de conseguir; o
servidor recebe apenas a sexta e conclui que está tudo bem.

Isso importa mais do que parece: é o sinal precoce de **rede degradando antes de
cair**. O momento em que um Wi-Fi começa a falhar de vez em quando é o momento
em que se conserta barato.

**Correção:** enviar `api_failures` e `post_latency_ms` no bloco de diagnóstico,
e implementar a regra do documento. `status: "ok"` passa a significar algo.

### S4 — Comandos que o dispositivo nunca recebeu não expiram

**Alta.** `jobs/watchdog.ts:66-70` expira apenas comandos em `sent`:

```ts
.where(and(eq(commands.status, "sent"), lt(commands.sentAt, expiredAt)))
```

Um comando em `queued` — enfileirado enquanto o dispositivo estava offline,
portanto nunca entregue — **fica em `queued` para sempre**. A seção 4 do
documento de observabilidade prevê exatamente esse caso e dá até o código:

> Se o dispositivo nunca chegou a receber (continua `queued` e não há POST há
> mais de 15 s), o evento é `cmd.device_offline` — são causas diferentes e o
> usuário precisa distinguir.

`cmd.device_offline` não aparece em lugar nenhum do servidor.

Duas consequências. A primeira é de interface: a mensagem "Dispositivo offline
desde 14:02 — comando aguardando na fila", listada no documento, não tem código
que a alimente. A segunda é operacional e pior: o dispositivo volta depois de
horas e **executa a fila inteira de decisões obsoletas**. A regra de supersede
protege comandos do mesmo alvo, mas não protege alvos diferentes — um
`device.reboot` de ontem ainda dispara.

**Correção:** no watchdog, marcar `queued` antigos com um TTL próprio (mais
generoso que o de `sent`, porque a espera é legítima enquanto o dispositivo está
fora) e emitir `cmd.device_offline`. Considerar também um teto de idade absoluto
para a entrega: comando com mais de N minutos não deve ser executado ao
religar.

### S5 — O rollup só olha para ontem

**Alta.** `jobs/maintenance.ts:128-129`:

```sql
where t.received_at >= date_trunc('day', now() - interval '1 day')
  and t.received_at <  date_trunc('day', now())
```

A janela é fixa no dia anterior. Se o servidor ficar fora do ar por três dias —
reboot do homelab, atualização que deu errado, queda de energia — esses três
dias **nunca são agregados**. E como `telemetry` é purgada por partição
enquanto `telemetry_hourly` é isenta, a perda vira permanente: a telemetria
bruta some e o rollup nunca existiu.

Isso ataca justamente a razão de o banco existir. `docs/plano-migracao-bettacare.md`
diz, sobre a tela de relatórios: *"Era o que não dava para fazer com MQTT
retido, e é a razão de o banco existir."*

**Correção:** agregar todas as horas ainda não agregadas, não a última janela
fixa. A consulta muda pouco — em vez do intervalo fixo, buscar de
`max(hour)` de `telemetry_hourly` até a hora corrente, com um teto de segurança
para a primeira execução não varrer meses de uma vez.

Como a inserção já é `on conflict do update`, reprocessar hora existente é
seguro. A propriedade que falta é só a **cobertura**.

### S6 — `"aquarium-01"` hardcoded

**Média.** `jobs/maintenance.ts:53` e `:74` gravam eventos de sistema com
`deviceId: "aquarium-01"` literal.

O resto do sistema evita cuidadosamente assumir um único dispositivo — o
comentário em `db/schema.ts:77` diz "Uma linha, na prática — mas nada aqui
assume isso". Estes dois pontos assumem, e assumem um slug específico.

Como `events.device_id` é `text` sem chave estrangeira (`db/schema.ts:248`), não
há erro: o evento é gravado sob um dispositivo que pode não existir e
simplesmente **não aparece na interface**, que filtra por dispositivo. Ou seja,
`db.partition_dropped` e `db.size_limit_unreachable` — dois eventos que o
usuário precisa ver — podem se perder em silêncio.

**Correção:** gravar por dispositivo real (`select device_id from devices`), ou
introduzir um `device_id` sentinela explícito para eventos de infraestrutura e
fazer a interface exibi-lo. A segunda opção é mais honesta: purga de partição
não é um fato sobre o aquário.

### S7 — `events.device_time` nunca é preenchido

**Média**, e é uma divergência real entre firmware e servidor.

O servidor lê `e.t` (`ingest/process.ts:326`):

```ts
deviceTime: e.t ? new Date(e.t) : null,
```

O firmware **nunca envia `t`**. Ele envia a hora local dentro do contexto
(`api_client.cpp:153-155`):

```cpp
e["ctx"]["device_time"] = eventos[i].time;   // "HH:MM"
```

E o comentário logo acima explica por quê: `t` espera um instante ISO completo e
o RTC só dá "HH:MM", sem data e sem fuso. A decisão do firmware é defensável. O
problema é que **o servidor não foi informado**: a coluna `events.device_time`
existe, tem comentário dizendo "Relógio do dispositivo; nulo se o RTC sumiu", e
é **sempre nula**.

O dado está lá, enterrado no JSONB do `ctx`, onde não é indexável nem ordenável.
E ele importa exatamente no caso que o comentário do firmware descreve: eventos
represados por horas de falta de rede chegam todos com o mesmo `received_at`, e
só o `device_time` diz quando cada um aconteceu.

**Correção:** decidir de que lado resolver. Ou o firmware envia `t` como ISO
completo (precisa da data do RTC, que ele tem), ou o servidor reconstrói o
`device_time` a partir de `ctx.device_time` + a data de `received_at`. A primeira
é mais limpa; a segunda não exige mexer no firmware. Em qualquer caso, **pare de
declarar uma coluna que ninguém preenche.**

### S8 — `ack` atrasado é jogado fora sem deixar rastro

**Média.** `ingest/process.ts:203`:

```ts
// Um ack para comando já resolvido é reenvio: ignorar em silêncio.
if (!cmd || (cmd.status !== "sent" && cmd.status !== "queued")) continue;
```

Para um `ack` genuinamente repetido, ignorar é certo. Mas o filtro não distingue
"repetido" de "atrasado", e o segundo caso é diagnóstico puro: um `ack` chegando
para um comando já marcado `expired` significa que **o dispositivo executou o
comando e a confirmação chegou depois do TTL**.

Isso diz duas coisas úteis: o comando funcionou (a interface está mentindo ao
dizer que expirou) e o `COMMAND_TTL_S` está curto demais para as condições reais
de rede. Hoje as duas informações são descartadas.

Combinado com o F1 do firmware — que perde o `ack` inteiro quando o POST falha —
este é o par que produz "o app disse que falhou mas a luz acendeu".

**Correção:** quando o comando estiver em `expired`, gravar um evento de servidor
`cmd.late_ack` com a diferença de tempo. Não mudar o status — o comando expirou
mesmo —, mas registrar que a expiração foi prematura.

### S9 — O histórico é mais pobre que o estado corrente

**Média.** `telemetry` (`db/schema.ts:202-231`) guarda nove campos: luz,
temperatura, validade, ventoinha (on/velocidade/rpm/modo) e RSSI.

Ficam **de fora do histórico**, existindo apenas como estado corrente:
`temp_age_ms`, `rtc_available`, `rtc_lost_power`, `wifi_reconnects`,
`config_version`, `light_source`.

O efeito é que perguntas de tendência não têm resposta. "Desde quando o RTC está
sem bateria?" só é respondível se houve um evento de transição — e se o
componente oscilou, o buffer de eventos do firmware pode ter descartado o começo
do episódio (F9 do firmware). O histórico deveria ser a rede de segurança dos
eventos, e hoje não é.

A DDL do S1 já inclui as colunas. O custo é pequeno: são booleanos e inteiros, e
a gravação continua sendo on-change.

### S10 — A primeira falha de um componente não vira evento

**Média.** `ingest/process.ts:290`:

```ts
if (changed && before !== undefined) {
```

A guarda `before !== undefined` evita ruído ao semear `component_status` no
primeiro contato, o que faz sentido. Mas ela também engole o caso real: um
dispositivo que **liga já com o sensor quebrado**.

A linha entra em `component_status` como `missing`, a aba Saúde mostra
corretamente, e a aba Logs não tem nada. Para quem está depurando, "não há
evento" e "não aconteceu nada" são indistinguíveis.

**Correção:** registrar evento na primeira avaliação quando o status inicial não
for `ok`. Semear `ok` continua silencioso.

### S11 — Sem limite de tamanho no corpo do ingest

**Média.** `ingest/router.ts:54` chama `c.req.json()` sem verificar
`Content-Length`. O corpo é bufferizado inteiro antes de o Zod ver qualquer
coisa.

A porta está na LAN, com token, então não é exposição pública. Mas o modelo de
ameaça relevante não é o invasor: é o **firmware com defeito**. Um `_build_body`
que entre em laço, um buffer de eventos corrompido, um `snprintf` sem terminador
— e o servidor de 8 GB compartilhado com o resto do homelab recebe um corpo de
tamanho arbitrário.

`docs/api-servidor.md` mede os corpos reais entre 391 e 777 bytes.

**Correção:** rejeitar com `413` acima de um teto generoso (16 KB cobre o pior
caso legítimo com folga de vinte vezes) **antes** de ler o corpo.

Vale também um limite de taxa por dispositivo. O intervalo mínimo aceito pelo
contrato é de 1 s (`device_config.cpp:39`); mais que isso é defeito.

### S12 — `fan.tach_stalled` pode ser falso positivo estrutural

**Média**, e é o mesmo achado do firmware visto do outro lado.
`health/evaluate.ts:112`:

```ts
const stalled = speed_percent > 0 && rpm === 0;
```

Com `STRIKES_TO_FAULT = 2` e POST a cada 3 s, o veredito vira `fault` em ~6 s.

A norma Intel de ventoinhas de 4 fios (seção 3.4, ver
[documento 02](02-hardware-e-ligacoes.md) §3) admite implementações em que a
ventoinha **desliga sozinha** abaixo do duty mínimo. Se a sua for desse tipo e o
duty mínimo dela for maior que os 30 % de `FAN_SPEED_LOW`, então `speed_percent
> 0 && rpm === 0` é o comportamento **correto** do hardware — e o sistema vai
gritar defeito a cada cooldown.

Um alarme que dispara sem defeito é pior que alarme nenhum: ensina a ignorar.

**Correção:** confirmar o tipo da ventoinha na bancada (medição 2 do roteiro do
documento 02). Se ela desligar abaixo do mínimo, a regra precisa de um piso —
só acusar defeito acima do duty mínimo conhecido.

### S13 e S14 — Achados menores

**S13** — `recordHealth` (`ingest/process.ts:263-286`) faz um `INSERT ... ON
CONFLICT` **por componente**, num laço, dentro da transação. Com os seis
vereditos atuais e um POST a cada 3 s, são 120 idas ao banco por minuto só para
saúde; com os oito propostos, 160. Somado ao resto, cada POST custa ~16
consultas.

Funciona folgadamente para um dispositivo — o documento de entrega mediu 40 MB
de RAM sob carga. Mas é o caminho quente, e um `INSERT` de múltiplas linhas com
um único `ON CONFLICT` resolveria em uma consulta. Vale fazer junto com a
ampliação do S2, não antes.

**S14** — `jobs/maintenance.ts:23` guarda `lastRunDay` em memória. Um reinício do
container depois das 03:15 faz a manutenção rodar de novo no mesmo dia. As três
operações são idempotentes — `ensureCurrentPartitions` é *ensure*, o rollup é
*upsert*, e a purga é dirigida por tamanho —, então não corrompe nada. O risco
real é pequeno: uma segunda passada de `enforceSizeLimit` no mesmo dia poderia
derrubar uma partição a mais se o banco estiver no limite. Persistir a marca numa
tabela resolveria; dado o teto de 1 GB ser rede de segurança e não regime
normal, é aceitável deixar como está e apenas registrar.

---

## O que está certo e não deve ser mexido

- **`ingest/auth.ts` inteiro.** SHA-256 dos dois lados antes do
  `timingSafeEqual` resolve o problema do comprimento variável de forma elegante.
  É o melhor arquivo do servidor.
- **A guarda de idempotência** (`ingest/process.ts:100-103`) e o raciocínio que a
  levou para `device_state` em vez de uma constraint única. O comentário em
  `db/schema.ts:138-146` explica corretamente por que o `UNIQUE` prometido em
  `respostas-bettacare.md` é impossível numa tabela particionada.
- **Aplicar os `ack` antes da checagem de duplicata**
  (`ingest/process.ts:91-94`). Sutil e correto: marcar comando confirmado é
  idempotente, e perder um `ack` custaria reentrega.
- **Comando não sai da fila ao ser entregue, só ao ser confirmado**
  (`ingest/process.ts:440-463`).
- **O rollup ponderado por duração** (`jobs/maintenance.ts:106-163`), e em
  especial o `coalesce` **dentro** do `least` — `least(60, NULL)` devolve 60 no
  PostgreSQL, e com o `coalesce` do lado de fora toda hora sem luz seria
  registrada como 60 minutos acesa. É uma armadilha real e o teste de CI a
  guarda.
- **Particionamento mensal com purga por `DROP`.** `DELETE` não devolve espaço
  ao sistema de arquivos e `VACUUM FULL` pega `ACCESS EXCLUSIVE` — num Postgres
  compartilhado, inaceitável. A modelagem está certa.
- **Índice BRIN em `telemetry.received_at`** (`db/schema.ts:227`): a tabela
  cresce em ordem de tempo, então o índice fica minúsculo. Escolha correta.
- **Índice parcial `commands_pending_idx`** (`db/schema.ts:331`), restrito a
  `queued`/`sent`.
- **Componentes viram `unknown` quando o dispositivo cai**
  (`jobs/watchdog.ts:56-60`). Manter `ok` com dado velho seria afirmar algo que
  não se sabe mais.
- **Configuração corrompida cai no padrão em vez de derrubar o ingest**
  (`ingest/process.ts:424-430`).
- **`temp_age_ms` como `bigint` e nulo** (`db/schema.ts:114-119`) — a correção do
  commit `a4596fe`, bem documentada no próprio schema.
- **Separação total dos dois roteadores** e o cabeçalho do Cloudflare Access nem
  sequer registrado na porta do ESP32 (`ingest/router.ts:13-22`).

---

## Plano de ação

### Etapa 1 — Contrato e schema do diagnóstico (S1, S9)

**Precede tudo do firmware.** Campo que o Zod não conhece é rejeitado no ingest.

1. Estender `packages/contract` com um bloco `diag` opcional no payload de
   telemetria. **Opcional**, para um firmware antigo continuar sendo aceito
   durante a transição.
2. Migration `0004` com as colunas do S1 e do S9 — **só adiciona coluna**, o que
   mantém a propriedade de reversão segura registrada em
   `docs/deploy-cli-homelab.md`.
3. Gravar em `upsertState` e na linha de `telemetry`.
4. Conferir que cada limite do Zod cabe na coluna correspondente. É a classe do
   bug de `a4596fe`, e a melhor hora de checar é ao acrescentar colunas.

**Critério de aceite:** um POST com o bloco `diag` completo é aceito, e um sem
ele também.

### Etapa 2 — As regras de saúde que faltam (S2, S3, S12)

1. `evaluateButton` e `evaluatePot`, alimentados pela Etapa 1.
2. Regra real de `api` a partir de `api_failures` e `post_latency_ms`.
3. Piso de duty na regra de `fan`, **depois** da medição de bancada.
4. Aproveitar e converter `recordHealth` para um `INSERT` multi-linha (S13).

**Critério de aceite:** desligar o potenciômetro do circuito faz `pot` aparecer
como `degraded` na aba Saúde em menos de um minuto.

### Etapa 3 — Ciclo de vida do comando (S4, S8)

1. TTL para comandos em `queued` e evento `cmd.device_offline`.
2. Teto de idade para entrega — não executar decisão de ontem.
3. Evento `cmd.late_ack` quando a confirmação chega depois da expiração.

**Critério de aceite:** com o dispositivo desligado, um comando enfileirado gera
`cmd.device_offline`, e a interface mostra a frase prevista no documento de
observabilidade.

### Etapa 4 — Integridade do histórico (S5, S6, S7, S10)

1. Rollup por cobertura, não por janela fixa.
2. Eliminar o `"aquarium-01"` hardcoded.
3. Resolver `device_time` — decidir o lado e implementar.
4. Registrar evento na primeira avaliação quando o status inicial não for `ok`.

**Critério de aceite:** derrubar o servidor por dois dias e voltar; os relatórios
das horas perdidas aparecem depois da manutenção seguinte.

### Etapa 5 — Robustez da fronteira (S11, S14)

1. Teto de 16 KB no corpo do ingest, verificado antes de ler.
2. Limite de taxa por dispositivo.
3. Persistir a marca da manutenção diária, se quiser fechar o S14.

---

## Dependências com as outras frentes

- A **Etapa 1 é pré-requisito** da Etapa 1 do [plano de firmware](03-plano-firmware.md).
  Contrato → banco → firmware, nessa ordem.
- A **Etapa 2 depende da medição de bancada** do
  [documento 02](02-hardware-e-ligacoes.md) para o piso de duty da ventoinha.
- A **Etapa 3 fecha o par** com o F1 e o F2 do firmware. Corrigir só um dos lados
  deixa o sintoma no ar: o firmware precisa parar de perder `ack`, e o servidor
  precisa parar de descartar os que chegam tarde.
- As migrations continuam **só aditivas**, o que preserva a garantia de rollback
  seguro que `docs/deploy-cli-homelab.md` registra. Vale manter essa propriedade
  — e se alguma etapa futura precisar quebrar, avisar o agente do homelab antes.
