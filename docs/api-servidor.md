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
| `GET /api/report` | série do rollup horário |
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
| Watchdog | 30 s | marca dispositivo offline, expira comandos sem `ack` |
| Rollup | a cada hora cheia | agrega a hora que acabou de fechar em `telemetry_hourly` |
| Manutenção | 1×/dia, 03:15 UTC | partições, rollup pendente, purga por tamanho |

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
elas contaminaram.

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
  limpando o -48 e refazendo a hora, duas vezes seguidas sem efeito na segunda
- corpos com `feeder: {connected: false}` e sem o bloco `feeder`, contra a
  imagem **v1.1.0** publicada: aceitos, sem correção nem evento — o firmware
  novo pode ir para o aquário antes do servidor novo
- sensor de temperatura ausente vira `missing`, veredito geral `critical`
- silêncio do dispositivo vira `offline`, componentes viram `unknown` e o
  comando pendente expira com `cmd.expired`
- rollup horário confere: 7 h de luz = 420 min, 2 h de ventoinha = 120 min
- purga derruba a partição antiga e preserva a do mês corrente e a do seguinte
