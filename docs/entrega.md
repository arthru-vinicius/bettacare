# Empacotamento e entrega

Referência da Fase 5.

---

## A imagem

`node:24-alpine`, uid `1000` (usuário `node`), `CMD node dist/main.js`.

**~59 MB** de transferência (comprimido) e **274 MB** em disco — destes, 234 MB
são a base oficial do Node e ~40 MB são a aplicação. Os dois números medem
coisas diferentes e é fácil citar um achando que é o outro.

Três estágios, e a separação é sobre cache: mexer no código não reinstala
`node_modules`, e é a instalação que domina o tempo do build.

| Estágio | Papel |
|---|---|
| `deps` | só os manifestos + `pnpm install`, com cache mount do store |
| `build` | `pnpm turbo build`, depois `pnpm deploy --prod` para podar |
| `runtime` | o mínimo: `dist`, `drizzle`, `web`, `node_modules` de produção |

O `pnpm deploy` existe porque os links de workspace de `@bettacare/contract`
apontariam para fora da imagem final sem ele.

### Duas armadilhas que só apareceram construindo

**`.tsbuildinfo` precisa sair junto com `dist` no `.dockerignore`.** É o cache
incremental do TypeScript. Vindo da máquina local, ele diz que tudo já foi
compilado, o `tsc` não emite nada — e o `dist` que ele supõe existir é
exatamente o que o `.dockerignore` acabou de excluir. O sintoma é um
`Cannot find module '@bettacare/contract'` três estágios depois da causa.

**O Turborepo filtra o ambiente.** Variável não declarada em `env` na
`turbo.json` não chega à tarefa. Sem `["BUILD_ID", "MIN_BUILD"]` declarados, o
carimbo de versão do PWA cairia no fallback de relógio e a **atualização
forçada nunca dispararia** — em silêncio, que é o pior jeito.

---

## Fixar limite de memória é requisito, não sugestão

Medido com a imagem final contra Postgres 17 real:

| Cenário | RAM |
|---|---|
| Com `--memory=256m`, repouso | 32 MB (12%) |
| Com `--memory=256m`, 300 POSTs + relatório de 30 dias | 40 MB (15%) |
| **Sem limite**, num host de 15,5 GB | **215 MB** |

Não é a aplicação consumindo mais: é o V8 dimensionando o heap pela memória
visível. Sem limite, o Node enxerga a RAM inteira do host e deixa o heap
crescer, porque nada o pressiona a coletar. Com o limite, ele lê o cgroup e se
comporta.

Isto corrigiu a estimativa de ~80 MB que estava em `respostas-bettacare.md`.

---

## Workflows

**`ci.yml`** — em push na `main` e em PR. Build, typecheck e testes contra um
Postgres 17 de serviço (o teste do rollup verifica uma regressão de SQL que
nenhum mock reproduziria). Constrói a imagem sem publicar, para uma quebra no
`Dockerfile` aparecer no PR e não na hora de marcar a tag.

**`release.yml`** — disparado por tag `v*`. Valida o formato
`vMAJOR.MINOR.PATCH` e falha cedo se estiver torto. Publica duas tags:

```
ghcr.io/arthru-vinicius/bettacare:v1.0.0
ghcr.io/arthru-vinicius/bettacare:sha-<commit>
```

**Nunca `latest`** — sem versão não há como saber o que está rodando nem voltar
atrás. Publicar é decisão explícita: não acontece por merge na `main`.

Para forçar a recarga da frota de PWAs num deploy de emergência, defina a
variável de repositório `MIN_BUILD` com o SHA do primeiro build aceitável.

---

## Como subir uma versão

```bash
git tag v1.0.0
git push origin v1.0.0
# a CI publica; depois, no servidor:
#   atualizar a tag em /opt/compose/bettacare/ e rodar docker compose up -d
```

A atualização do container é manual por decisão — Watchtower está proibido no
stack.

---

## Verificado

Com a imagem final rodando contra Postgres 17:

- `/healthz` responde 200 nas duas portas;
- a UI é servida com `Cache-Control: no-store` no HTML, `sw.js`, `version.json`
  e manifest; `immutable` nos assets com hash;
- `BUILD_ID` e `MIN_BUILD` chegam ao `version.json` **e** ao `sw.js`;
- o ingest aceita o POST do ESP32 com token válido;
- **isolamento entre as portas**: a UI responde 404 na porta do ESP32 e o
  endpoint de telemetria responde 404 na porta da UI;
- `SIGTERM` encerra em **542 ms** com código 0 — sem a espera de dez segundos
  no `docker compose down`;
- uid `1000`, migrations e export do PWA presentes na imagem.

## Verificado em produção (2026-10-02)

- o push para o `ghcr.io` pela CI: v1.1.0, v1.1.1 e v1.1.2, cada uma com CI e
  Release verdes e a imagem conferida por dentro;
- o deploy pelo `homelab update`, com as migrations aplicadas numa transação
  e conferidas pelo `homelab-c4` contra o banco;
- o app atrás do Cloudflare Tunnel e do Access: é assim que ele é usado, e a
  mudança de configuração de 2026-10-02 ficou registrada com o e-mail que
  chegou no cabeçalho do Access.

## Não verificado

- restart do container após reboot do servidor com o Postgres ainda subindo —
  o backoff foi exercitado só em desenvolvimento.
