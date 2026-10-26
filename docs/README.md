# BettaCare

Controlador de aquário para um peixe betta, feito com **observabilidade como
requisito**: além de acender a luminária e ligar a ventoinha, o sistema sabe
dizer *por que* alguma coisa não funcionou.

Um ESP32 lê a temperatura, comanda a luminária (por SSR) e a ventoinha de 4
fios com tacômetro e fala por HTTP com um servidor Node (Hono + PostgreSQL), que
serve o app (PWA). Um módulo opcional, com ESP32-C3 próprio, dosa a ração um
grão por vez.

```
 ESP32 (firmware/bettacare) ── POST /api/v1/telemetry ──► :8080 ┐
   │ UART                                                       ├─ servidor Hono ── PostgreSQL
 módulo alimentador (ESP32-C3)                         :3000 ───┘   (um processo, duas portas)
                                                         ▲
                                              PWA (Next.js, export estático)
```

## Documentos

| Documento | O que cobre |
|---|---|
| [`arquitetura.md`](./arquitetura.md) | O porquê do desenho: eventos, saúde por componente, ciclo de vida do comando, ingest, retenção |
| [`api.md`](./api.md) | Referência das rotas HTTP, autenticação, erros, cabeçalhos de cache |
| [`interface.md`](./interface.md) | O PWA: telas, atualização forçada, feedback de comando |
| [`firmware.md`](./firmware.md) | O firmware do ESP32 principal: núcleos, rede, sensores, verificação |
| [`hardware/controlador-principal.md`](./hardware/controlador-principal.md) | Pinagem, ligações e montagem do ESP32 principal |
| [`hardware/modulo-alimentador.md`](./hardware/modulo-alimentador.md) | Pinagem e montagem do módulo alimentador |
| [`entrega.md`](./entrega.md) | Ferramental, imagem Docker, CI e releases |

Cada pasta de firmware tem o próprio README: [`feeder-module`](../firmware/feeder-module/README.md)
(módulo do alimentador), [`bettacare-debug`](../firmware/bettacare-debug/README.md)
(produção mais instrumentação, para investigar defeitos) e
[`test`](../firmware/test/README.md) (testes de host).

## Código

| Pasta | O que é |
|---|---|
| `apps/server` | Servidor Hono: ingest do ESP32, API do app, jobs, migrations (Drizzle) |
| `apps/web` | O PWA, em Next.js com `output: 'export'` |
| `packages/contract` | O contrato Zod compartilhado pelas três pontas (servidor, app e firmware) |
| `firmware/bettacare` | Firmware de produção do ESP32 principal |
| `firmware/feeder-module` | Firmware do módulo alimentador (ESP32-C3) |
| `firmware/bettacare-debug` | Produção mais instrumentação, gravável por OTA |
| `firmware/test` | Testes de host do firmware, com `g++` no PC |

## Desenvolvimento

```sh
pnpm install
pnpm db:up          # Postgres de desenvolvimento (docker-compose.dev.yml)
pnpm db:migrate     # aplica as migrations
pnpm dev            # servidor e app
pnpm typecheck && pnpm test
```

As variáveis do servidor estão em `.env.example`. Os firmwares leem a
configuração de um `config.h` por pasta, criado a partir do `config.example.h`
— o `config.h` fica fora do git.
