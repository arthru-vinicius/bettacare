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
| `PUT /api/settings` | grava e incrementa a versão |
| `GET /api/report` | série do rollup horário |

### `GET /api/events`

| Parâmetro | Exemplo | Nota |
|---|---|---|
| `sev` | `error,fatal` | lista separada por vírgula |
| `comp` | `temp,fan` | idem |
| `source` | `device` ou `server` | |
| `q` | `sensor` | busca em `msg` e `code` |
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
| Manutenção | 1×/dia, 03:15 UTC | partições, rollup horário, purga por tamanho |

Quando o dispositivo é dado como offline, **todos os componentes viram
`unknown`** — manter `ok` com dado velho seria afirmar algo que não se sabe
mais.

---

## Verificado de ponta a ponta

O que foi exercitado contra Postgres 17 real, simulando o ESP32 com `curl`:

- reenvio do mesmo `seq` não duplica linha nem corrompe estado
- comando entregue na resposta do POST e confirmado no POST seguinte
- recusa do dispositivo (`ack.ok = false`) vira `rejected` com o código do erro
- ventoinha com PWM e tacômetro em zero vira `fault` no segundo ciclo, e volta
  a `ok` quando gira
- luminária que não obedece vira `fault` no segundo ciclo divergente
- sensor de temperatura ausente vira `missing`, veredito geral `critical`
- silêncio do dispositivo vira `offline`, componentes viram `unknown` e o
  comando pendente expira com `cmd.expired`
- rollup horário confere: 7 h de luz = 420 min, 2 h de ventoinha = 120 min
- purga derruba a partição antiga e preserva a do mês corrente e a do seguinte
