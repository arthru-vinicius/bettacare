# =============================================================================
# BettaCare — imagem de produção
#
# Construída em CI, nunca no servidor: são 2 núcleos competindo com o Postgres,
# o cloudflared e o resto do homelab, e um build de frontend ali levaria
# dezenas de minutos.
#
#   docker build -t ghcr.io/arthru-vinicius/bettacare:v1.0.0 .
# =============================================================================

# -----------------------------------------------------------------------------
# 1. Dependências
#
# Separada do build para aproveitar o cache: mexer no código não reinstala
# node_modules, e é a instalação que domina o tempo.
# -----------------------------------------------------------------------------
FROM node:24-alpine AS deps
WORKDIR /repo

RUN corepack enable

COPY package.json pnpm-lock.yaml pnpm-workspace.yaml .npmrc ./
COPY apps/server/package.json           apps/server/
COPY apps/web/package.json              apps/web/
COPY packages/contract/package.json     packages/contract/

RUN --mount=type=cache,id=pnpm,target=/pnpm/store \
    pnpm config set store-dir /pnpm/store && \
    pnpm install --frozen-lockfile

# -----------------------------------------------------------------------------
# 2. Build
# -----------------------------------------------------------------------------
FROM node:24-alpine AS build
WORKDIR /repo

RUN corepack enable

COPY --from=deps /repo/node_modules                    ./node_modules
COPY --from=deps /repo/apps/server/node_modules        ./apps/server/node_modules
COPY --from=deps /repo/apps/web/node_modules           ./apps/web/node_modules
COPY --from=deps /repo/packages/contract/node_modules  ./packages/contract/node_modules
COPY . .

# O carimbo de versão do PWA. Sem ele o `stamp-version.mjs` cai no git (ausente
# aqui) e depois no relógio — funciona, mas o identificador deixa de apontar
# para um commit. A CI passa o SHA.
ARG BUILD_ID=""
ENV BUILD_ID=$BUILD_ID

# Recarga forçada da frota. Vazio no caminho normal; preenchido só quando uma
# versão quebrada foi ao ar ou o contrato da API mudou de forma incompatível.
ARG MIN_BUILD=""
ENV MIN_BUILD=$MIN_BUILD

RUN pnpm turbo build

# Poda para produção. `pnpm deploy` resolve os links do workspace numa árvore
# autocontida — sem isso, os symlinks de `@bettacare/contract` apontariam para
# fora da imagem final.
RUN pnpm --filter @bettacare/server deploy --prod --legacy /prod

# -----------------------------------------------------------------------------
# 3. Runtime
# -----------------------------------------------------------------------------
FROM node:24-alpine AS runtime
WORKDIR /app

ENV NODE_ENV=production

COPY --from=build --chown=node:node /prod/node_modules      ./node_modules
COPY --from=build --chown=node:node /prod/dist              ./dist
# As migrations são arquivos, não código compilado: precisam viajar junto ou o
# migrator sobe sem nada a aplicar.
COPY --from=build --chown=node:node /repo/apps/server/drizzle ./drizzle
COPY --from=build --chown=node:node /repo/apps/web/out        ./web

# Caminho absoluto em vez de relativo ao módulo: o layout aqui não é o do
# repositório, e um relativo fixo daria uma UI em 503 sem pista do motivo.
ENV WEB_ROOT=/app/web

# uid 1000 (usuário `node`, que já vem na imagem oficial). Arquivos de segredo
# montados no container precisam ser legíveis por ele — este item já quebrou
# dois serviços neste servidor e é a causa menos óbvia de falha em produção.
USER node

EXPOSE 3000 8080

# Sem HEALTHCHECK aqui de propósito: quem decide o healthcheck é o compose do
# homelab, apontando para GET /healthz. Embutir um na imagem duplicaria a
# configuração em dois lugares que podem divergir.

# O Node é o PID 1 e trata SIGTERM explicitamente, então não é preciso `tini`
# nem `init: true` no compose.
CMD ["node", "dist/main.js"]
