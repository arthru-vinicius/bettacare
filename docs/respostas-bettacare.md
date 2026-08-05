# Respostas do bettacare ao contrato do homelab

Resposta ponto a ponto ao checklist da seção 9 de `requisitos-bettacare.md`.
Escrito do lado da aplicação, para o agente que vai escrever a role Ansible.

> **Nota**: este repositório é público. Endereços reais foram substituídos por
> marcadores como `<IP_DO_SERVIDOR>`. Os valores concretos ficam fora do
> versionamento.

> **Contexto**: o bettacare é uma reescrita completa. A aplicação anterior
> (PHP + MQTT, sem banco) é descartada; só o firmware do ESP32 é aproveitado
> como base, e mesmo ele troca o transporte de MQTT para HTTP.

---

## 0. O que mudou desde a primeira entrega deste documento

A implementação está pronta e os números abaixo são **medidos**, não estimados.
Cinco pontos divergem da versão anterior deste documento e valem leitura antes
de escrever a role:

| Mudou | Antes | Agora |
|---|---|---|
| Comando de build | `npm ci && npm run build` | pnpm via corepack — seção 1 |
| **Limite de memória** | "256 MB é confortável" | **é obrigatório fixar um** — seção 7 |
| Retenção | por dias (`TELEMETRY_RETENTION_DAYS`) | por tamanho, 1 GiB (`DB_SIZE_LIMIT_BYTES`) — seção 5 |
| Campo `ack` do ESP32 | `[91]` | `[{id, ok, code?}]` — seção 4 |
| Env vars | — | três novas, uma removida — seção 6 |

Nada disso muda porta, uid, healthcheck ou o desenho de rede. O impacto na
infraestrutura é uma linha de `deploy.resources.limits.memory` no compose.

---

## 1. Empacotamento

| Item | Valor |
|---|---|
| Repositório | `https://github.com/arthru-vinicius/bettacare` |
| Branch de produção | `main` |
| Visibilidade | **público** — não é necessário token do `ghcr.io` no vault |
| Imagem | `ghcr.io/arthru-vinicius/bettacare:v<semver>` |
| Plataforma | `linux/amd64` |
| Runtime | Node.js 24 LTS (`node:24-alpine`) |
| Build | GitHub Actions, multi-stage, tag `v<semver>` a partir de git tag |

**Sobre a visibilidade**: o repositório é **público**, por decisão do usuário —
é um projeto aberto. Não há segredo no código: toda configuração vem de variável
de ambiente, e o único arquivo com credenciais (`firmware/*/config.h`) está no
`.gitignore`. **Nenhum token de leitura do `ghcr.io` precisa entrar no vault.**

### uid do processo dentro do container

**O processo roda como uid `1000`, gid `1000`** (usuário `node`, que já vem na
imagem oficial do Node). Não é distroless, não é `nonroot` do Google.

O `Dockerfile` termina com `USER node`, e nada roda como root em runtime.
Arquivos de segredo montados no container precisam ser legíveis por uid 1000.

### Comandos

O monorepo é gerenciado por **pnpm workspaces + Turborepo**. O `Dockerfile`
já encapsula tudo — o servidor não precisa conhecer nenhum destes comandos,
que rodam só dentro do build da CI:

```bash
# build (roda na CI, nunca no servidor)
corepack enable
pnpm install --frozen-lockfile
pnpm turbo build       # export estático do Next + bundle do servidor

# start (entrypoint do container, já no CMD da imagem)
node dist/main.js
```

Imagem final em `node:24-alpine`, uid `1000`. Contém apenas o bundle do
servidor, as migrations, o export do PWA e as dependências de produção.

| | Tamanho |
|---|---|
| Transferência (comprimido, o que o servidor baixa) | **~59 MB** |
| Em disco, descomprimido | **274 MB** |
| — dos quais são a base `node:24-alpine` | 234 MB |
| — dos quais são a aplicação | **~40 MB** |

Registro os dois porque medem coisas diferentes e a confusão entre eles é fácil:
o primeiro é o que passa pela rede, o segundo é o que ocupa no NVMe. Se outros
serviços do homelab já usarem `node:24-alpine`, as camadas da base são
compartilhadas e o custo marginal cai para os ~40 MB.

---

## 2. Rede

Um único processo Node escutando em **duas portas**, com dois roteadores Hono
independentes. Não há proxy interno, não há Next.js server, não há segundo
container.

| Público | Porta interna | Publicar no host? |
|---|---|---|
| Interface web + API do navegador | `3000` | **não** — `cloudflared` alcança por `http://bettacare:3000` |
| Ingestão do ESP32 | `8080` | **sim** — `<IP_DO_SERVIDOR>:8080:8080` |

Portas configuráveis por `PORT_UI` e `PORT_INGEST`, com esses valores como padrão.

Conformidade com a seção 3 do contrato:

- HTTP puro nas duas portas, sem TLS, sem ACME.
- **Nenhum redirect HTTP→HTTPS** em lugar nenhum do código.
- `X-Forwarded-Proto` é honrado onde houver URL absoluta. Na prática a aplicação
  **não constrói nenhuma URL absoluta**: a UI é estática e fala com a própria
  origem por caminho relativo, e o `start_url` do manifest PWA é `/`.
- As duas portas são servidas por roteadores separados. Uma requisição que chegue
  na 8080 não tem acesso a nenhuma rota da UI, e vice-versa.
- O endpoint do ESP32 **não deve entrar nas regras de ingress do túnel**.

---

## 3. O sentido servidor → ESP32

**Escolhida a Opção A**, na variante que economiza metade das requisições:

> O ESP32 faz um único `POST /api/v1/telemetry` a cada N segundos. A **resposta
> desse POST** carrega os comandos pendentes.

Um request serve os dois sentidos. O servidor **nunca inicia conexão** com o
dispositivo, então o problema de endereço deixa de existir — não importa qual IP
o ESP32 tenha, e nada precisa ser feito na infraestrutura.

A latência de comando é o intervalo de envio (3 s por padrão). Para luz e
ventoinha isso é imperceptível.

O intervalo é **ajustável pelo servidor**, não pelo firmware: ele viaja no campo
`telemetry_interval_ms` da configuração devolvida ao dispositivo. Se o volume de
requisições incomodar, dá para subir para 10 s sem regravar o ESP32.

---

## 4. Contrato do endpoint de telemetria

**`POST http://<IP_DO_SERVIDOR>:8080/api/v1/telemetry`**

Autenticação por header `X-Api-Token` (o mesmo cabeçalho que o firmware já usa),
comparado com `crypto.timingSafeEqual` — **comparação em tempo constante**, como
o contrato exige. Sem TLS, como acordado.

### Corpo enviado pelo dispositivo

```json
{
  "device_id": "aquarium-01",
  "boot_id": 42,
  "seq": 1337,
  "uptime_ms": 84213000,
  "device_time": "2026-08-04T17:31:02Z",
  "config_version": 7,
  "ack": [{ "id": 91, "ok": false, "code": "light.gpio_fault" }],
  "light":       { "on": true },
  "temperature": { "celsius": 26.5, "available": true, "valid": true },
  "fan":         { "on": false, "speed_percent": 0, "rpm": 0, "mode": "auto" },
  "rtc":         { "available": true, "time": "14:32" },
  "wifi":        { "rssi": -58, "ip": "<IP_DO_ESP32>" },
  "events": [
    { "t": "2026-08-04T17:30:55Z", "m": "[RTC] Automacao: ligando luminaria" }
  ]
}
```

### Resposta do servidor

Corpo pequeno e de tamanho previsível, como pede o contrato — tipicamente
menos de 300 bytes:

```json
{
  "ok": true,
  "server_time": "2026-08-04T17:31:03Z",
  "config_version": 8,
  "config": {
    "light_on_time": "10:00",
    "light_off_time": "17:00",
    "fan_trigger_c": 29.0,
    "fan_off_c": 27.5,
    "telemetry_interval_ms": 3000
  },
  "commands": [ { "id": 91, "action": "light.set", "on": true } ]
}
```

O bloco `config` **só é enviado quando `config_version` do servidor diverge do
que o dispositivo reportou**. No caso normal ele é omitido e a resposta fica em
torno de 100 bytes.

### Tolerância a reenvio

A guarda é o `seq`: `device_state` guarda `last_boot_id` e `last_seq`, e um POST
cujo `seq` não avança dentro do mesmo `boot_id` é descartado. Uma checagem de
uma linha, por chave primária.

> **Correção em relação à versão anterior deste documento.** Havíamos prometido
> `UNIQUE (device_id, boot_id, seq)`. Isso **não é possível**: `telemetry` virou
> tabela particionada (ver seção 5), e o PostgreSQL exige que a chave de
> partição entre em toda constraint única. A alternativa —
> `UNIQUE (received_at, device_id, boot_id, seq)` — enfraqueceria a garantia
> sem entregar nada, porque o mesmo `seq` reenviado poderia entrar duas vezes se
> as tentativas caíssem em meses diferentes. A guarda por `last_seq` cobre o
> reenvio real, que acontece em segundos.

Reenviar o mesmo POST:

- não duplica linha de histórico
- não corrompe o estado corrente
- **devolve a mesma lista de comandos** — um comando só sai da fila quando o
  dispositivo o confirma no campo `ack` de um POST seguinte, não quando é
  entregue. Se a resposta se perder, o comando é reentregue.

Cada `action` é idempotente por construção: são `light.set`, `fan.set_speed`,
`fan.set_mode`, `config.apply` e `device.reboot` — estados desejados, não
alternâncias. O `light.toggle` do sistema antigo foi eliminado justamente
porque reentrega de um toggle inverte o estado duas vezes.

### O `ack` deixou de ser uma lista de números

Também uma correção: prevíamos `"ack": [91]`, que só sabe dizer "recebi" e
nunca "não consegui". Agora é `[{id, ok, code?}]`, e quando `ok` é falso o
`code` traz o motivo (`light.gpio_fault`, `cmd.unsupported`). É dele que sai a
mensagem de erro que o usuário lê na interface. Custa alguns bytes por comando
confirmado, e comandos são raros.

### Carimbo de horário

Guardados os dois, como o contrato pede: `received_at` (carimbado pelo servidor,
autoritativo) e `device_time` (o que o RTC do ESP32 acha que é, para diagnóstico
de deriva). Ambos `timestamptz`, sempre em UTC.

### Frequência

Duas frequências diferentes, e a distinção importa para o volume do banco:

| | Frequência | Volume/dia |
|---|---|---|
| Requisições HTTP | a cada **3 s** | ~28.800 req |
| Linhas em `telemetry` | on-change + heartbeat de 60 s | ~2.000 linhas |

Ou seja: o dispositivo fala a cada 3 s, mas o banco só ganha linha quando algo
muda ou quando passa um minuto sem mudança. Isso dá da ordem de **730 mil linhas
por ano**, algo como 150 MB com índices — sem pressão sobre o cluster.

### Retenção: por tamanho, não por idade

Mudou em relação à versão anterior. O usuário pediu um teto rígido: o database
`bettacare` **nunca passa de 1 GiB** (`DB_SIZE_LIMIT_BYTES`, ajustável).

O detalhe que obrigou a mudar o desenho: `DELETE` no PostgreSQL **não devolve
espaço ao sistema de arquivos**. As linhas viram tuplas mortas, o `VACUUM`
comum apenas marca páginas como reutilizáveis, e `pg_database_size()` não se
move. Um job que apagasse linhas antigas e medisse depois entraria em laço até
esvaziar a tabela. `VACUUM FULL` devolveria o espaço, mas pega
`ACCESS EXCLUSIVE` e precisa do dobro do tamanho da tabela em disco livre —
inaceitável num cluster compartilhado.

O que devolve espaço de imediato é `DROP TABLE`. Por isso **`telemetry` e
`events` são particionadas por mês** (`PARTITION BY RANGE (received_at)`), e a
purga derruba a partição mais antiga inteira. Instantânea, sem lock na tabela
pai.

`telemetry_hourly`, `settings`, `commands`, `devices` e `component_status` são
isentos: somam poucos megabytes e são o que mantém os relatórios funcionando
sobre períodos cuja telemetria bruta já foi descartada.

Com o volume acima, o teto de 1 GiB só seria alcançado por volta do sexto ano —
na prática ele é uma **rede de segurança** contra um componente defeituoso que
comece a gerar eventos em rajada, não o regime normal.

**Nada disso exige ação da infraestrutura.** O particionamento é criado pelas
migrations, e a purga roda dentro do processo. Só registro aqui porque muda o
comportamento de crescimento do database no cluster compartilhado.

---

## 5. Banco de dados

Conexão por `DATABASE_URL`, nunca embutida na imagem.

- **Pool máximo de 4 conexões** (`PG_POOL_MAX`, padrão 4). São três usuários e um
  dispositivo; um pool grande só tiraria RAM do cache do Postgres.
- **Todos os timestamps são `timestamptz` gravados em UTC.** O fuso só aparece
  na apresentação, e vem de `TZ_DISPLAY` (padrão `America/Recife`) — o processo
  nunca depende do fuso do sistema operacional.
- A aplicação **sobe mesmo com o Postgres indisponível**: os listeners HTTP
  abrem primeiro, e a conexão com o banco é tentada em background com backoff.
  Enquanto não houver banco, `/healthz` responde `503` e o endpoint do ESP32
  responde `503` com corpo curto (o dispositivo continua operando sozinho).

### Extensões

**Nenhuma.** Não é necessária nenhuma extensão do PostgreSQL.

Vale registrar o que foi considerado e descartado: `TimescaleDB` exigiria
instalação de pacote e superusuário para um ganho que não se justifica neste
volume; `pgcrypto` é desnecessário porque `gen_random_uuid()` é função de core
desde o PostgreSQL 13. Agregação para relatórios é SQL puro com índice BRIN em
`received_at`.

### Migrations

**Automáticas na subida**, via `drizzle-orm/node-postgres/migrator`.

- Idempotentes: o migrator mantém a tabela de controle `__drizzle_migrations` e
  aplica apenas o que falta. Rodar o mesmo compose dez vezes não falha nem
  duplica.
- Se o banco não estiver pronto, entram na mesma retentativa com backoff da
  conexão — não derrubam o processo.
- Existe também o comando manual documentado `npm run db:migrate`, para o caso
  de precisar aplicar fora do ciclo de subida.

### Schema (resumo)

Sete tabelas, modeladas para o hardware que existe hoje (luminária, ventoinha
com tacômetro e sensor de temperatura):

| Tabela | Papel |
|---|---|
| `devices` | cadastro do ESP32, `last_seen_at`, IP reportado, versão de firmware |
| `device_state` | **uma linha por dispositivo** — o estado corrente que as mensagens retidas do MQTT forneciam |
| `telemetry` | histórico bruto, `UNIQUE (device_id, boot_id, seq)` |
| `telemetry_hourly` | rollup horário (min/média/máx de temperatura, minutos de luz e de ventoinha) |
| `events` | os logs do firmware, que hoje morrem num buffer circular de 30 entradas em RAM |
| `commands` | fila de comandos com `delivered_at` / `acked_at` |
| `settings` | horários e limiares, com `config_version` — fonte de verdade da configuração |

Sobre `settings`: com o banco no jogo, a configuração passa a ter o servidor
como fonte de verdade, e o NVS do ESP32 vira cache offline. O dispositivo
compara o `config_version` a cada POST e aplica quando diverge. Ele continua
operando sozinho sem rede, com a última configuração conhecida.

---

## 6. Configuração e segredos

### Variáveis de runtime

| Variável | Obrigatória | Segredo | Padrão |
|---|---|---|---|
| `DATABASE_URL` | sim | **sim** | — |
| `DEVICE_INGEST_TOKEN` | sim | **sim** | — |
| `PORT_UI` | não | não | `3000` |
| `PORT_INGEST` | não | não | `8080` |
| `NODE_ENV` | não | não | `production` |
| `LOG_LEVEL` | não | não | `info` |
| `TZ_DISPLAY` | não | não | `America/Recife` |
| `PG_POOL_MAX` | não | não | `4` |
| `DB_SIZE_LIMIT_BYTES` | não | não | `1073741824` (1 GiB) |
| `DEVICE_OFFLINE_AFTER_S` | não | não | `60` |
| `COMMAND_TTL_S` | não | não | `90` |
| `WEB_ROOT` | não | não | `/app/web` (já definido no `Dockerfile`) |
| `ALLOWED_USER_EMAILS` | não | não | vazio (aceita qualquer e-mail que o Access autenticar) |

`TELEMETRY_RETENTION_DAYS` **deixou de existir** — a retenção passou a ser por
tamanho. Se ela já tiver entrado no playbook, remova; a aplicação a ignora.

Há um `.env.example` na raiz do repositório com todas elas comentadas.

`DEVICE_INGEST_TOKEN` é o valor esperado no header `X-Api-Token` do ESP32.

`ALLOWED_USER_EMAILS` é defesa em profundidade opcional: o Cloudflare Access já
barra quem não deve entrar, e essa lista só existe para o caso de a policy do
Access ser afrouxada por engano. Vazia, a aplicação confia inteiramente no Access.

### Variáveis de build time

**Nenhuma que o servidor precise conhecer.**

Esse ponto costuma dar problema com Next.js, então vale ser explícito: a
interface é um **export estático** que conversa com a própria origem por caminho
relativo. Não existe nenhuma `NEXT_PUBLIC_*` com host, URL ou segredo que
precise ser congelada no bundle. A CI não recebe nenhuma variável de
configuração para buildar a imagem — o mesmo artefato roda em qualquer host.

### Persistência

**A aplicação é stateless além do banco.** Nenhum diretório precisa sobreviver
ao container. Não há upload, sessão em disco, cache em arquivo ou artefato
gerado em runtime. Os ícones do PWA vão dentro da imagem.

Nada a montar a partir de `/srv/bettacare/`.

---

## 7. Saúde, desligamento e consumo

### Healthcheck

**`GET /healthz`**, exposto nas duas portas, sem autenticação.

- `200` com `{"status":"ok"}` quando a aplicação está pronta para servir
- `503` quando o banco está inacessível ou as migrations ainda não terminaram
- Executa apenas `SELECT 1` sobre uma conexão já existente do pool, com timeout
  de 2 s — sem consulta pesada e sem efeito colateral

### Desligamento

`SIGTERM` fecha os dois listeners, aguarda as requisições em voo (teto de 5 s),
encerra o pool do Postgres e sai com código 0. Não deve haver espera de dez
segundos no `docker compose down`.

Como o processo Node é o PID 1 e trata o sinal explicitamente, não é necessário
`init: true` nem `tini` no compose.

### Logs

`stdout` e `stderr`, uma linha JSON por evento (pino). Nada é escrito em arquivo
dentro do container.

### Consumo — **fixar um limite de memória é obrigatório**

Isto corrige a estimativa da versão anterior. Os números abaixo são medidos, com
a imagem final rodando contra um Postgres 17 real.

| Cenário | RAM |
|---|---|
| **Com `--memory=256m`**, em repouso | **32 MB** (12% do limite) |
| **Com `--memory=256m`**, após 300 POSTs + relatório de 30 dias | **40 MB** (15%) |
| **Sem limite**, em repouso, num host de 15,5 GB | **215 MB** |

CPU desprezível nos três casos: 0,02% no pico.

A diferença não é a aplicação consumindo mais — é o **V8 dimensionando o heap
pela memória visível**. Sem `deploy.resources.limits.memory` no compose, o Node
enxerga a RAM inteira do host e deixa o heap crescer bem além do necessário,
porque nada o pressiona a coletar. Com o limite, ele lê o cgroup e se comporta.

**Recomendação: fixar 256 MB.** Sobra folga de 6× sobre o pico medido, e é o
que faz a aplicação caber no orçamento de um servidor de 8 GB compartilhado.
Não recomendo abaixo de 192 MB sem medir de novo.

```yaml
deploy:
  resources:
    limits:
      memory: 256M
```

### Processos de segundo plano

Dois, ambos `setInterval` dentro do próprio processo. Não há worker separado,
fila, nem cron do sistema.

| Job | Frequência | Custo |
|---|---|---|
| Watchdog de dispositivo offline | a cada 30 s | um `UPDATE` numa linha |
| Rollup horário + purga de retenção | 1x/dia, 03:15 | poucos segundos de agregação |

O rollup roda de madrugada justamente para não competir por CPU em horário de uso.

---

## 8. Segurança

### Autenticação de humanos

A aplicação **não implementa login próprio**. A identidade vem do header
`Cf-Access-Authenticated-User-Email`, injetado pelo Cloudflare Access, e é usada
para atribuir autoria nas tabelas `commands` e `settings` — dá para saber quem
ligou a luz e quem mudou o horário.

Não há tela de login, recuperação de senha, sessão ou cookie de autenticação.

### O header não é confiado onde não deve

Na porta `8080` (ESP32) o header `Cf-Access-Authenticated-User-Email` é
**ignorado incondicionalmente**, mesmo se presente. Aquele tráfego não passa
pelo Access, e lá a autenticação é exclusivamente o token. São roteadores Hono
distintos: a porta 8080 sequer registra o middleware que lê esse header.

### Service token do Access

**Não é necessário no momento.** Nenhum sistema externo consome a API — o ESP32
entra pela LAN, não pelo túnel. Se no futuro houver integração máquina a máquina
(um Home Assistant, por exemplo), aí sim seria o caminho, e avisamos antes.

---

## 9. Checklist da seção 9, respondido

- [x] **Repositório, branch, privado?** — `arthru-vinicius/bettacare`, `main`, **público** (sem token no vault)
- [x] **Build e start** — `pnpm install --frozen-lockfile && pnpm turbo build` / `node dist/main.js` (ambos já no `Dockerfile`)
- [x] **uid do processo** — `1000:1000` (usuário `node`)
- [x] **Porta interna da UI** — `3000`, não publicada
- [x] **Porta do ESP32** — `8080`, publicada em `<IP_DO_SERVIDOR>:8080:8080`
- [x] **Variáveis de ambiente** — tabela na seção 6; nenhuma de build time
- [x] **Healthcheck** — `GET /healthz`
- [x] **Migrations** — automáticas na subida, idempotentes
- [x] **Extensões do PostgreSQL** — nenhuma
- [x] **Persistência** — stateless além do banco
- [x] **Contrato de telemetria** — seção 4
- [x] **Opção da seção 4.4** — Opção A, com comandos na resposta do POST
- [x] **RAM e background** — 32–40 MB **com limite de 256 MB fixado**, que é requisito e não sugestão; dois `setInterval` internos
- [x] **Service token do Access** — não necessário
