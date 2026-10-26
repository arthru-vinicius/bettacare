# Arquitetura do servidor

O servidor guarda o estado do aquário e, principalmente, **a razão pela qual
alguma coisa não funcionou**: saber o estado atual não basta. Três perguntas
concretas guiaram o desenho. Se a implementação não responder as três, ela não
está pronta:

1. *Por que a luminária não acendeu quando eu apertei o botão no app?*
2. *Desde quando o sensor de temperatura sumiu?*
3. *O aquário está saudável agora, componente por componente?*

Este documento explica o **porquê** do desenho. As rotas e os formatos estão
em [`api.md`](./api.md); a interface, em [`interface.md`](./interface.md); o
firmware, em [`firmware.md`](./firmware.md).

---

## 1. Eventos estruturados

Texto livre (`"[RTC] Automacao: ligando luminaria"`) não dá para filtrar,
agregar nem alertar. Por isso o evento é um registro com campos:

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

O firmware deduplica por `(comp, code)` e com contador: em vez de N linhas
iguais, uma linha com `repeat_count`. Um sensor oscilando não pode inundar o banco.

---

## 2. Saúde por componente

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
| `temp` (DS18B20) | nenhum device no barramento 1-Wire | CRC inválido, ou leitura *stale* > 30 s | — |
| `rtc` (DS3231) | I²C não responde | `lost_power` (bateria) | — |
| `fan` | — | modo de segurança (temperatura indisponível) | **duty > 0 e tacômetro em 0 por ≥ 5 s** |
| `light` (SSR) | — | — | **estado ≠ o do último comando do app por ≥ 5 s** |
| `wifi` | — | RSSI < -80 dBm | — |
| `button` | — | preso pressionado ≥ 30 s | — |
| `pot` | — | — | — |
| `api` | — | o POST anterior falhou | — |
| `nvs` | — | falha de escrita desde o boot | — |
| `ota` | — | — | a última atualização falhou |
| `system` | — | heap < 40 KB, ou eventos perdidos | reinício não comandado (brownout, panic, watchdog) |

O código é `apps/server/src/health/evaluate.ts`; esta tabela é o resumo. Três
linhas pedem explicação:

- **`light`** só conta enquanto a última mudança da luz foi o próprio
  comando. Botão físico e automação também mudam a luz, e isso não é falha.
- **`pot`** só mostra a leitura. Nenhum valor isolado é defeito: o mínimo é
  como se desliga a ventoinha, e o topo do curso satura o ADC em 4095 em
  qualquer montagem boa. Até a v1.1.0, "≥ 4090" contava como
  mau contato e dava alarme falso com o potenciômetro funcionando.
- **`api`** não tem `fault`: se o POST chegou, a comunicação funciona agora. O
  que `api_failures` mostra é quantas tentativas falharam antes desta.

Duas linhas dessa tabela merecem atenção porque são as que respondem à pergunta
*"por que não consigo controlar?"*:

**Ventoinha em `fault`** — o tacômetro é o único componente com realimentação
real. Duty acima de zero e RPM zerado significa fisicamente uma coisa só: a
ventoinha não está girando. Cabo solto, rolamento travado ou fonte caída. Sem o
tacômetro vigiado, isso seria invisível.

**Luminária em `fault`** — o SSR não tem retorno, então a única evidência
possível é a divergência entre o que foi comandado e o que o firmware reporta.
Se o servidor mandou `light.set{on:true}`, o dispositivo confirmou o comando, e
dois POSTs depois `light.on` continua `false`, alguma coisa está errada entre o
GPIO e a lâmpada. É um diagnóstico honesto: não afirma *qual* é o defeito, mas
afirma com segurança que existe um.

### Quando o aquário fica mudo

Quando o dispositivo é dado como offline, **todos os componentes viram
`unknown`** — manter `ok` com dado velho seria afirmar algo que não se sabe
mais.

### Medida por tempo, não por POST

Anomalia vira `fault` quando **persiste por tempo**, não por número de POSTs:
5 s para ventoinha com PWM e tacômetro em zero e para luminária divergente,
30 s para botão preso. O início fica no `detail` do componente
(`anomaly_since`). Com POSTs contados, o intervalo de 1 s transformaria toda
partida da ventoinha — o tacômetro conta em janelas de 2 s — em falha com push.

A divergência da luminária só conta com a última mudança vinda do comando
(`source: "command"`): o botão físico e a automação mudam a luz de propósito,
e antes isso virava "luminária não respondeu".

`component_status` só é regravado quando muda status, código ou início de
anomalia — ou a cada 5 s, para o valor exibido não envelhecer.

### Avisos que não ficam para sempre

- **`system.event_overflow`** (eventos descartados) é um **episódio**. O
  contador do firmware só zera no reinício, e contado como estado ele deixou o
  Sistema "instável" por uma semana depois de uma queda de 21 h. Agora o status
  vale por 1 h a partir da última vez que o contador subiu, e o aviso diz o que
  houve — o buffer de 24 eventos encheu sem rede —, em vez de culpar um
  componente em rajada.
- **"Voltou ao normal" só depois de problema de verdade.** O watchdog zera o
  status de todos os componentes para `unknown` quando o aquário fica mudo; a
  volta de `unknown` para `ok` não é recuperação de nada (o `device.online` já
  conta a volta) e não gera mais evento — eram onze por queda de contato, 880 de
  9.600 eventos em oito dias.
- **`ingest lento`**: um `warn` no log do servidor quando processar um POST
  passa de 1 s. O firmware mede a latência inteira (rádio + servidor) e não
  sabe dividir; isto separa as duas metades.

---

## 3. Ciclo de vida do comando

Esta é a resposta direta a *"qual erro não está me permitindo controlar a
luminária"*. Sem um ciclo de vida, um comando some no éter e não há onde olhar.

```
queued ──► sent ──► acked        (deu certo)
   │        │
   │        ├────► rejected      (dispositivo recusou, com código)
   │        └────► expired       (sem ack dentro do TTL)
   └─────────────► superseded    (comando mais novo para o mesmo alvo)
```

### O `ack` carrega o desfecho

Uma lista de ids (`"ack": [91]`) só sabe dizer *"recebi"*, nunca
*"não consegui"*. Por isso o `ack` é:

```jsonc
"ack": [
  { "id": 91, "ok": true },
  { "id": 92, "ok": false, "code": "light.gpio_fault" }
]
```

O custo no ESP32 é de alguns bytes por comando confirmado, e comandos são
raros. O ganho é que a falha volta com nome.

### Quem decide `expired`

O watchdog de 30 s, com um prazo de vida contado da **criação** do comando: `COMMAND_MAX_AGE_S` (padrão 15 min). Até ele, o comando
espera o aquário e é reentregue a cada POST — inclusive depois de `sent`, porque
`sent` só quer dizer "foi numa resposta", e com o Wi-Fi do aquário a resposta se
perde com frequência. Vencido o prazo:

- `sent` sem `ack` vira `expired` com `cmd.expired` (pode ter sido executado; se
  a confirmação aparecer depois, vira o evento `cmd.late_ack`). Quem foi
  entregue no último minuto ganha mais 2 min de folga para o `ack` voltar.
- `queued` vira `expired` com `cmd.stale_discarded`: nunca chegou ao aquário.

Enquanto continua `queued` e não há POST há mais de 15 s, o evento é
`cmd.device_offline` — a fila esperando, não uma falha.

Até a v1.2.0 o prazo era `COMMAND_TTL_S`, 90 s contados da primeira entrega:
uma resposta perdida logo antes de uma queda bastava para o comando morrer, já
executado ou não. Os 15 min vieram do histórico de produção (2026-10-07 a 14):
87% das 103 quedas duraram menos que isso.

**Reinício no meio.** O firmware 2.1.1 reconhece uma reentrega pelo `id` e
confirma sem executar de novo — mas perde essa memória ao reiniciar. Por isso,
quando o `boot_id` muda com `feeder.feed_now` ou `device.reboot` ainda `sent`,
o servidor não reentrega: o reboot é confirmado (aconteceu), e a alimentação
encerrada como `cmd.interrupted`, para não dobrar a ração.

### O que a interface mostra

Cada falha chega ao usuário como uma frase própria, vinda de um `code`
distinto e não de heurística no front — "Dispositivo offline desde 14:02 —
comando aguardando na fila", "O dispositivo recusou: falha no GPIO da
luminária", "Comando expirou sem confirmação". A tabela completa está em
[`interface.md`](./interface.md#feedback-de-comando).

---

## 4. Processos em segundo plano

Dois `setInterval` dentro do próprio processo — o do watchdog e o da
manutenção, que também dispara o rollup a cada hora cheia. Não há worker, fila
nem cron do sistema.

| Job | Frequência | O que faz |
|---|---|---|
| Watchdog | 30 s | marca dispositivo offline (60 s de silêncio), notifica o celular só depois de `DEVICE_OFFLINE_ALERT_AFTER_S` (15 min), encerra comandos que passaram do prazo de vida |
| Rollup | a cada hora cheia | agrega a hora que acabou de fechar em `telemetry_hourly` |
| Manutenção | 1×/dia, 03:15 UTC | partições, rollup pendente, poda dos acionamentos, purga por tamanho |

---

## 5. Tolerância a dado ruim

Um valor ruim isolado não pode derrubar o POST inteiro nem contaminar o
histórico. O ingest corrige o que dá, descarta o que não dá e registra a
correção.

### Corpo com problema

Valor fora de faixa é grampeado, formato inválido em `rtc.time`, `wifi.ip` e
`device_time` vira `null`, e um **item** inválido de `events` ou
`diagnostic.checks` é descartado sozinho (`ingest.event_dropped`) — antes, um
evento de componente que o servidor não conhecia derrubava o POST inteiro. O
log da recusa leva o valor recebido em cada campo.

O mesmo campo corrigido de novo dentro de 1 h não abre outro
`ingest.field_rejected`: soma `repeat_count` no evento aberto (o "×N" da aba
Registros), e só a abertura sai como `warn` no log; as repetições vão para
`debug`. Um campo que o firmware manda errado erra em todo POST — na v1.1.0
foram 86 mil eventos por dia com a telemetria a 1 s.

### Temperatura

Duas faixas, no contrato (`TEMP_PLAUSIBLE_C` e `TEMP_USUAL_C`), escolhidas
para esta instalação — aquário em Recife, com ar-condicionado:

- **Fora de 10–45 °C, a leitura é impossível.** O firmware 2.0.2 já a
  descarta; o ingest confere de novo, para firmware anterior e regressão. O
  POST segue como se trouxesse a última leitura boa — o valor não chega ao
  estado, ao histórico, à saúde nem ao gráfico — e o descarte vira
  `temp.implausible`, agrupado por hora como as correções. Foi um -48,00 °C
  isolado em produção, um quadro de 1-Wire que passou no CRC.
- **Fora de 16–33 °C, a leitura é real, mas rara.** O rollup horário, que
  alimenta o gráfico, só agrega essa faixa. O episódio vira **um** aviso
  `temp.out_of_usual_range` nos Registros: aberto na primeira leitura fora,
  reescrito a cada minuto ou pico novo ("Água acima de 33 °C há 12 min"),
  fechado na primeira de volta — "Água ficou acima de 33 °C por 11 min, das
  12:02 às 12:13 (máx. 34,6 °C)". O caso comum, água na faixa agora e no POST
  anterior, não custa consulta nenhuma.

A migration 0010 tirou do histórico as duas leituras de -48 e refez a hora que
elas contaminaram — só o impossível: uma hora antiga com calor ou frio de
verdade fica como estava, porque para ela não existe o aviso que a regra nova
deixaria no lugar.

### Bloco `feeder`

`connected` é o único campo que conta sempre. Com `connected: false`, o resto
do bloco é ignorado antes de validar, e o servidor mantém a última agenda
conhecida — desconectar é o estado normal de um módulo que não fica ligado o
tempo todo. Sem o bloco, vale o mesmo: `connected` falso e a última agenda
preservada. O firmware até a 2.0.0 mandava o bloco inteiro zerado quando o
módulo nunca tinha respondido.

`state.feeder` no `/overview` é `null` enquanto nenhum módulo foi detectado, e
é isso que faz o app mostrar "nenhum módulo" em vez de uma agenda vazia. A
migration 0009 limpou, uma vez, a agenda 00h/00h que os zeros tinham gravado
em produção.

**Refeições em 24 h (v1.2.0, firmware 2.1.0).** O bloco traz `meals_24h`:
quantas refeições o módulo contou nas últimas 24 h, somando agenda, botão e
app. A migration 0011 acrescenta a coluna `device_state.feeder_meals_24h`, e o
`/overview` a devolve em `state.feeder.meals_24h` — é o "2 de 3" do app. O
limite (`FEEDER_MEALS_PER_24H` = 3, no contrato) é do **módulo**, não do
servidor: o servidor só repassa o `feeder.feed_now` com `force: true` quando o
usuário confirma o "alimentar mesmo assim".

Três avisos do alimentador viram **push**, além de irem para os Registros:
`feeder.limit_reached` (uma refeição — da agenda, do botão ou do app —
recusada pelo limite), `feeder.hopper_empty` (nenhum grão caiu: reservatório
vazio ou doseador travado) e `feeder.sensor_fault` (autoteste do sensor
reprovado; a refeição saiu contada pelo servo). Uma notificação por código: a
nova substitui a anterior. `feeder.limit_overridden`, `feeder.feed_denied` e
`feeder.meal_missed` ficam só nos Registros.

---

## 6. Acionamentos da ventoinha (`fan_runs`)

Uma linha por **rodada**: de quando a ventoinha ligou a quando desligou, com a
temperatura de partida, o pico, o mínimo e a de saída. O ingest a mantém POST a
POST, olhando só a transição de `fan.on` contra o estado anterior
(`ingest/fan-runs.ts`, função pura `planFanRun`):

| Antes → agora | O que acontece |
|---|---|
| desligada → ligada | abre a rodada, com o horário do POST |
| ligada → desligada | fecha, com o horário do POST e a temperatura de saída |
| ligada → ligada | só atualiza pico, mínimo, potência e "alguém mexeu" — e só quando o ingest já grava uma linha de histórico (mudança ou heartbeat), não a cada POST |
| ligada → ligada com `boot_id` novo | o PWM zera no reset: fecha a velha no instante do reinício (`uptime` atrás) e abre outra |

**Silêncio.** Passados 5 min entre dois POSTs, o que aconteceu no meio é
desconhecido. Uma rodada que abre depois disso tem o início no primeiro POST que
a viu; uma que fecha depois disso termina na **última vez que se viu ligada**
(um limite inferior, não "agora") e fica sem temperatura de saída. As duas
levam `approx`, e o app marca com "≈".

**Não cresce sem limite.** A manutenção diária apaga o que passa de
`FAN_RUNS_RETENTION_DAYS` (90 dias) e o que excede `FAN_RUNS_MAX_ROWS` (20 mil
por dispositivo) — nunca a rodada aberta. É o "dado antigo é sobrescrito" desta
tabela: o Postgres reaproveita o espaço do que sai, então o tamanho fica estável
em vez de acompanhar a idade da instalação. Não é isenta da purga como o
`telemetry_hourly`: tem a poda por tempo dela.

A migration 0013 cria a tabela (um índice parcial garante no máximo uma rodada
aberta por aparelho) e **reconstrói uma vez** as rodadas que já estavam na
telemetria bruta, para o histórico não nascer vazio no dia do deploy. Só lê
`telemetry` e só escreve na tabela nova; a versão anterior da imagem a ignora.

---

## 7. Retenção: o teto de 1 GB

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
**isentos**. `fan_runs` (v1.4.0) não é derrubada por partição, mas tem a poda
por tempo e por linhas dela na mesma manutenção diária (90 dias, 20 mil linhas
por dispositivo): são dezenas de linhas por dia no pior caso, e o espaço do que
sai é reaproveitado. Somados dão poucos megabytes e são justamente o que dá valor ao
histórico longo: a curva de temperatura do ano passado sobrevive mesmo depois
de a telemetria bruta daquele mês ter sido descartada.

### Uma consequência do particionamento

O PostgreSQL exige que toda constraint `UNIQUE` de uma tabela particionada
contenha a chave de partição. Ou seja, o `UNIQUE (device_id, boot_id, seq)`
inicialmente previsto **não é possível** como está — teria de
virar `UNIQUE (received_at, device_id, boot_id, seq)`, o que enfraquece a
garantia: o mesmo `seq` reenviado poderia entrar duas vezes se as duas
tentativas caíssem em meses diferentes.

A idempotência migra para onde ela é mais barata: `device_state` guarda
`last_seq` e `last_boot_id`, e o ingest descarta o POST cujo `seq` não avança.
É uma checagem de uma linha, com índice pela PK, e cobre o caso do reenvio de
verdade — que acontece em segundos, não em meses.

### Volume esperado

Com gravação **on-change + heartbeat de 60 s** (a decisão de projeto), são cerca de
2.000 linhas/dia, ~150 MB/ano com índices. O teto de 1 GB só seria alcançado por
volta do sexto ano — na prática ele é uma **rede de segurança** contra um
componente defeituoso que comece a gerar eventos em rajada, não o regime normal
de operação. Ainda assim precisa funcionar, porque o cenário que ele cobre é
exatamente aquele em que ninguém está olhando.

---

## 8. Verificado de ponta a ponta

O que foi exercitado contra Postgres 17 real, simulando o ESP32 com `curl`:

- reenvio do mesmo `seq` não duplica linha nem corrompe estado
- comando entregue na resposta do POST e confirmado no POST seguinte
- recusa do dispositivo (`ack.ok = false`) vira `rejected` com o código do erro
- ventoinha com PWM e tacômetro em zero vira `fault` depois de 5 s, e volta
  a `ok` quando gira; recém-ligada, com o tacômetro ainda em zero, não vira
- luminária trocada pelo botão físico depois de um comando não vira falha
- configuração gravada, registrada com quem mudou, aplicada pelo aquário e
  refletida em `state.config_version`; gravação idêntica não sobe a versão
- planilhas de registros e de medições com BOM, `;`, vírgula decimal e fuso de
  exibição
- evento de componente desconhecido descartado sozinho, o resto do POST aceito
- a mesma correção em 30 POSTs vira um evento só com `repeat_count` 30, e
  abre outro passada a janela de 1 h
- bloco do alimentador zerado (firmware 2.0.0) não gera evento nem agenda, e
  desconectar preserva a última agenda conhecida
- leitura de -48 °C: o estado fica com a última boa, o histórico e a saúde
  nunca a veem, e o aviso soma repetições; sem leitura boa anterior, vira
  leitura inválida, não "sensor perdido"
- água acima de 33 °C por 11 min: um aviso só, com duração, horário no fuso
  de exibição e pico; abaixo de 16 °C, outro, no sentido contrário
- rollup com amostras de 15 e 34 °C agregando só as de 27 e 28 °C; a 0010
  limpando o -48 e refazendo só as horas que ele contaminou — o 34 real de
  uma hora fica —, duas vezes seguidas sem efeito na segunda
- corpos com `feeder: {connected: false}` e sem o bloco `feeder`, contra a
  imagem **v1.1.0** publicada: aceitos, sem correção nem evento — o firmware
  novo pode ir para o aquário antes do servidor novo
- sensor de temperatura ausente vira `missing`, veredito geral `critical`
- silêncio do dispositivo vira `offline`, componentes viram `unknown` e o
  comando pendente expira com `cmd.expired`
- rollup horário confere: 7 h de luz = 420 min, 2 h de ventoinha = 120 min
- purga derruba a partição antiga e preserva a do mês corrente e a do seguinte
