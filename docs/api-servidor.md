# API do servidor

Referência das duas superfícies HTTP do BettaCare. Escrita para quem vai
implementar o firmware (Fase 3) e a interface (Fase 4).

Os schemas são a autoridade: tudo aqui é validado por `@bettacare/contract`.

---

## Porta 8080 — o ESP32

Publicada com bind explícito no IP da LAN. **Não entra nas regras de ingress do
túnel** — um ESP32 atrás do Cloudflare Access receberia uma página de login e
não saberia o que fazer com ela.

| Rota | O quê |
|---|---|
| `GET /healthz` | `200` pronto, `503` sem banco. Sem autenticação. |
| `POST /api/v1/telemetry` | O único endpoint que o firmware usa. |

Autenticação: header `X-Api-Token`, comparado em tempo constante. Os dois lados
passam por SHA-256 antes da comparação, então o **comprimento** do token
recebido também não vaza pelo tempo de resposta.

A conexão fica aberta entre POSTs (keep-alive de 75 s, `main.ts`): o firmware
2.1.1 a reaproveita, e num Wi-Fi que perde pacote abrir uma conexão TCP por
POST era a parte mais frágil do caminho.

### Respostas de erro

| Código | Corpo | Quando |
|---|---|---|
| `401` | `{"ok":false,"error":"unauthorized"}` | token ausente ou errado |
| `400` | `{"ok":false,"error":"invalid_json"}` | corpo não é JSON |
| `400` | `{"ok":false,"error":"invalid_body"}` | não passou no contrato |
| `503` | `{"ok":false,"error":"db_unavailable"}` | banco fora do ar |
| `500` | `{"ok":false,"error":"internal"}` | falha inesperada |

Em `503` o dispositivo **continua operando sozinho** com a última configuração
do NVS. É o cenário normal logo depois de um reboot do servidor.

### Tamanhos medidos

| Caso | Bytes |
|---|---|
| Resposta normal (config em dia, sem comando) | **85** |
| Com um comando pendente | **121** |
| Com o bloco `config` completo | ~290 |

O contrato do homelab pede corpo pequeno e previsível; o teto que assumimos é
300 bytes.

---

## Porta 3000 — a interface

Não publicada no host: o `cloudflared` alcança por `http://bettacare:3000`.
Serve o export estático do Next e a API do navegador.

Identidade vem do header `Cf-Access-Authenticated-User-Email`, injetado pelo
Cloudflare Access. **Não há login próprio.** Na porta 8080 esse header é
ignorado incondicionalmente, mesmo se presente — aquele tráfego não passa pelo
Access.

| Rota | O quê |
|---|---|
| `GET /healthz` | igual à 8080, sem autenticação |
| `GET /api/me` | e-mail do usuário autenticado |
| `GET /api/overview` | estado corrente + saúde por componente + veredito geral |
| `GET /api/events` | logs paginados por cursor |
| `GET /api/commands` | histórico de comandos com a trilha inteira |
| `POST /api/commands` | enfileira um comando (`202`) |
| `GET /api/settings` | configuração e `config_version` |
| `PUT /api/settings` | grava, incrementa a versão e registra `settings.updated` |
| `GET /api/history/fan` | temperatura minuto a minuto e acionamentos da ventoinha de um período (aba Ventoinha) |
| `GET /api/report` | série do rollup horário (a aba Luminária usa para o tempo aceso) |
| `GET /api/export/events.csv` | planilha dos registros, com os filtros de `/api/events` |
| `GET /api/export/telemetry.csv` | planilha das medições de um período |

### `GET /api/events`

| Parâmetro | Exemplo | Nota |
|---|---|---|
| `sev` | `error,fatal` | lista separada por vírgula |
| `comp` | `temp,fan` | idem |
| `source` | `device` ou `server` | |
| `q` | `sensor` | busca em `msg` e `code`; `%` e `_` contam como texto |
| `from` | `2026-10-02T03:00:00Z` | início do período, inclusivo |
| `to` | `2026-10-03T03:00:00Z` | fim do período, exclusivo |
| `limit` | `50` | teto de 200 |
| `cursor` | `<iso>\|<uuid>` | vem do `next_cursor` da página anterior |

Paginação por **cursor**, não por offset: a tabela recebe linhas o tempo todo, e
com offset uma inserção entre duas páginas faria o usuário ver o mesmo evento
duas vezes ou pular um.

### `GET /api/history/fan`

Alimenta a aba Ventoinha: a curva de temperatura e a tabela de acionamentos de
um período, numa resposta só — os dois cartões sempre falam do mesmo intervalo.

| Parâmetro | Valores | Nota |
|---|---|---|
| `range` | `24h` (padrão), `7d`, `30d` | qualquer outro → `400 invalid_range` |

```jsonc
{
  "range": "24h",
  "from": "2026-10-21T04:44:00.000Z", "to": "2026-10-22T04:44:20.000Z",
  "bucket_s": 60,            // largura de cada ponto
  "gap_s": 150,              // silêncio que interrompe a linha
  "points": [[1792557840, 24.75, 24.70, 24.80], …],   // [início s Unix, média, mín, máx]
  "temperature": { "min": 23.4, "min_at": "…", "avg": 25.9, "max": 29.0, "max_at": "…" },
  "runs": [{ "id": 12, "started_at": "…", "ended_at": null, "duration_s": 1380,
             "mode": "auto", "manual": false, "speed_max": 60,
             "temp_start": 29.1, "temp_end": null, "temp_peak": 29.3, "temp_low": 28.4,
             "approx": false }],
  "runs_total": 3,
  "summary": { "runs": 3, "on_seconds": 6720, "avg_duration_s": 2700, "avg_drop_c": 1.5 }
}
```

- **Vem da telemetria bruta**, não do rollup horário: o rollup só tem uma média
  por hora, e "qual era a temperatura às 14:35?" pede mais resolução. A
  resolução de cada período (`HISTORY_SPEC`, `apps/server/src/api/history.ts`):
  24 h a 1 min (≤ 1.440 pontos), 7 dias a 15 min, 30 dias a 1 h. A linha de
  gravação é on-change + heartbeat de 60 s, então em 24 h cada minuto com
  leitura é praticamente a leitura daquele instante. Os pontos de 7 e 30 dias
  trazem mínima e máxima, para um pico curto não sumir na média.
- **Só entra leitura fresca e plausível** (`temp_valid`, 10–45 °C). Uma leitura
  velha repetida não é medição do instante.
- **Buracos ficam buracos.** Intervalo sem nenhuma linha não vira ponto; o
  cliente interrompe a linha quando a distância entre dois pontos passa de
  `gap_s` (2,5 intervalos) em vez de ligar uma temperatura que ninguém mediu.
- `temperature` são os extremos **exatos** do período (e o instante da
  primeira vez que ocorreram), não os do intervalo agregado: "máxima 28,8 às
  14:35" tem de bater com a linha.
- `runs`: as rodadas que **tocam** o período, mais recentes primeiro, até 500
  (`runs_total` diz quantas existem). Rodada aberta (`ended_at: null`) tem a
  duração até agora, se o aquário está falando; senão, até a última vez em que
  se viu a ventoinha ligada.

### `POST /api/commands`

Corpo é uma ação do contrato:

```jsonc
{"action":"light.set","on":true}
{"action":"fan.set_speed","percent":60}
{"action":"fan.set_mode","mode":"auto"}
{"action":"device.reboot"}
```

Responde **`202`**, não `200`: o comando foi aceito, mas só será executado
quando o dispositivo buscar a fila no próximo POST. A interface acompanha o
desfecho pelo `status` em `GET /api/commands`, não por esta resposta.

Comandos do mesmo alvo se anulam — enfileirar um novo marca os anteriores como
`superseded`. Sem isso, um dispositivo voltando de meia hora offline executaria
em sequência uma fila de decisões já obsoletas.

Um comando vive `COMMAND_MAX_AGE_S` (15 min) a partir da criação: até lá ele é
(re)entregue a cada POST, mesmo depois de `sent`, até o `ack` chegar. O
`GET /api/commands` traz `expires_at` — até quando ele ainda pode ser
executado — para a interface dizer "na fila até 14:17". Vencido, vira
`expired` com `cmd.stale_discarded` (nunca entregue) ou `cmd.expired`
(entregue sem confirmação). Quando o dispositivo reinicia com
`feeder.feed_now` ou `device.reboot` ainda `sent`, eles não são reentregues: o
reboot é confirmado e a alimentação encerrada como `cmd.interrupted`.

### `PUT /api/settings`

Corpo é a configuração **inteira** (`deviceConfigSchema`): as regras cruzam
campos — desliga abaixo de liga, acende diferente de apaga —, então o servidor
valida o conjunto. A interface manda o que já existe com só a parte editada
trocada.

- Gravação idêntica à atual não sobe a versão: o aquário não reaplica à toa.
- Toda mudança vira o evento `settings.updated`, com o que mudou em frase
  ("Luminária: acende 09:30 e apaga 18:15 (era 10:00–17:00)") e quem mudou.
- O `GET /api/overview` traz `state.config_version`, a versão que o **aquário**
  diz estar usando. Quando ela alcança a da raiz, a mudança foi aplicada — é o
  que permite à interface dizer "aplicado", e não só "salvo".

### `GET /api/export/*.csv`

Download direto pelo navegador (os cookies do Access vão junto). `from`/`to`
como em `/api/events`; sem eles, os últimos 7 dias. Período máximo de 400 dias,
teto de 200 mil linhas.

Formato do Excel em português: separador `;`, vírgula decimal, BOM de UTF-8 e
datas no fuso de `TZ_DISPLAY`, mais uma coluna em UTC. Texto vindo do firmware
que começa com `=`, `+`, `-` ou `@` sai com apóstrofo, para não virar fórmula.
Gerado em lotes de mil linhas e enviado em fluxo — o container tem teto de
256 MB.

---

## Cabeçalhos de cache

Onde a briga da atualização do PWA é ganha. Como o Hono serve os arquivos, eles
são nossos — sem depender de CDN nem de proxy, que aliás estão proibidos.

| Recurso | `Cache-Control` |
|---|---|
| `/_next/static/**` | `public, max-age=31536000, immutable` |
| `/sw.js`, `/version.json`, `*.html`, `/` | `no-store, must-revalidate` |
| resto | `public, max-age=0, must-revalidate` |

---

## Processos de segundo plano

Dois `setInterval` dentro do próprio processo. Não há worker, fila nem cron do
sistema.

| Job | Frequência | O que faz |
|---|---|---|
| Watchdog | 30 s | marca dispositivo offline (60 s de silêncio), notifica o celular só depois de `DEVICE_OFFLINE_ALERT_AFTER_S` (15 min), encerra comandos que passaram do prazo de vida |
| Rollup | a cada hora cheia | agrega a hora que acabou de fechar em `telemetry_hourly` |
| Manutenção | 1×/dia, 03:15 UTC | partições, rollup pendente, poda dos acionamentos, purga por tamanho |

Quando o dispositivo é dado como offline, **todos os componentes viram
`unknown`** — manter `ok` com dado velho seria afirmar algo que não se sabe
mais.

### Saúde medida por tempo

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

### Acionamentos da ventoinha (`fan_runs`, v1.4.0)

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

### Saúde: avisos que não ficam para sempre

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

## Verificado de ponta a ponta

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
