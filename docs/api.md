# API HTTP do servidor

Referência das duas superfícies HTTP do BettaCare: a porta do ESP32 (8080) e a
da interface (3000). Os schemas são a autoridade — tudo aqui é validado por
`@bettacare/contract`. O comportamento por trás das rotas (saúde, ciclo dos
comandos, ingest, retenção) está em [`arquitetura.md`](./arquitetura.md).

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

O ESP32 tem pouca memória e a resposta precisa ser pequena e previsível; o teto
que assumimos é 300 bytes.

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

Um comando vive `COMMAND_MAX_AGE_S` (15 min) a partir da criação e é
(re)entregue a cada POST até o `ack` chegar. O `GET /api/commands` traz
`expires_at` — até quando ele ainda pode ser executado — para a interface dizer
"na fila até 14:17". Os estados finais e seus códigos (`cmd.expired`,
`cmd.stale_discarded`, `cmd.interrupted`) estão em
[`arquitetura.md`](./arquitetura.md#3-ciclo-de-vida-do-comando).

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
são nossos — sem depender de CDN nem de proxy. A lógica do lado do navegador
está em [`interface.md`](./interface.md#atualização-forçada).

| Recurso | `Cache-Control` |
|---|---|
| `/_next/static/**` | `public, max-age=31536000, immutable` |
| `/sw.js`, `/version.json`, `*.html`, `/` | `no-store, must-revalidate` |
| resto | `public, max-age=0, must-revalidate` |
