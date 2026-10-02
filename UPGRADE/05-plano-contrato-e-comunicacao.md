# Contrato e comunicação — achados e plano de ação

Auditoria de `packages/contract/src/**` confrontado com o que o firmware
realmente serializa (`api_client.cpp`, `event_log.cpp`) e com o que o servidor
realmente lê (`ingest/process.ts`, `ingest/router.ts`).

**Veredito geral:** o contrato é a peça mais bem pensada do projeto. A decisão de
declarar um teto numérico em todo inteiro — documentada em `primitives.ts:88-100`
com a história do bug que a motivou — é exemplar. O código de evento
deliberadamente **não** ser um enum fechado é a escolha certa para um sistema com
firmware e servidor versionados em separado.

E, ainda assim, ele contém o achado mais grave desta auditoria inteira.

---

## C1 — Um único valor fora de faixa derruba a telemetria inteira

**Crítica.** Este é o achado que mais importa em todo o `UPGRADE/`.

### O mecanismo

`telemetryRequestSchema` valida o corpo como um todo. Em `ingest/router.ts:59-66`:

```ts
const parsed = telemetryRequestSchema.safeParse(raw);
if (!parsed.success) {
  return c.json({ ok: false, error: "invalid_body" }, 400);
}
```

Falhou uma validação, **o corpo inteiro é recusado**. E o corpo inteiro carrega
o estado do aquário, os eventos acumulados desde o último POST e os `ack` dos
comandos executados.

Do lado do firmware, `api_client.cpp:300-304` classifica o 400 como
`API_HTTP_ERROR`, o que incrementa `_failures`, o que aciona o recuo progressivo
(`net_task.cpp:65-71`) até **30 segundos** entre tentativas. O `seq` não avança,
então cada tentativa reenvia rigorosamente o mesmo corpo inválido — para sempre.

### O gatilho mais provável

`telemetry.ts:64`:

```ts
rpm: z.int().min(0).max(20000),
```

Vinte mil RPM são 40.000 pulsos por minuto, ou 667 Hz. Agora releia a §4 do
[documento 02](02-hardware-e-ligacoes.md): o tacômetro está ligado apenas ao
pull-up interno de ~45 kΩ do ESP32, com bordas lentas, ao lado de um cabo que
chaveia a 25 kHz. **Ruído nessa linha produz contagem espúria com folga de
sobra para passar de 667 Hz.**

A cadeia completa, então, é esta:

```
resistor de pull-up ausente  (custo: R$ 0,10)
        ↓
tacômetro conta ruído
        ↓
rpm > 20000
        ↓
Zod rejeita o corpo inteiro → HTTP 400
        ↓
firmware recua para 30 s e reenvia o mesmo corpo, indefinidamente
        ↓
watchdog do servidor: device.offline
        ↓
todos os componentes viram unknown
        ↓
o app diz "controlador sem contato"
```

O aquário está funcionando perfeitamente. A luz acende no horário, a ventoinha
segue a temperatura, o botão responde. E o sistema inteiro de observabilidade
afirma que o dispositivo sumiu — pela única razão de que ele tentou **contar
o que estava acontecendo**.

Há um segundo detalhe que torna o diagnóstico ainda mais confuso:
`bettacare.ino:104` faz `s.fan_rpm = (uint16_t)fan_get_rpm()`. Um RPM de 70.000
trunca para 4.464 — um número plausível. Então o defeito aparece de forma
**intermitente**: às vezes o valor truncado cai abaixo de 20.000 e o POST passa,
às vezes não. Intermitente, sem log, e o log é justamente o que não consegue
chegar.

### O mesmo mecanismo, outros gatilhos

`rpm` é o mais provável, mas não é único. Todo campo com faixa fechada é uma
porta para a mesma falha:

| Campo | Faixa | Gatilho plausível |
|---|---|---|
| `fan.rpm` | 0–20000 | ruído no tacômetro (§4 do doc 02) |
| `temperature.celsius` | -55 a 125 | DS18B20 com CRC ruim devolvendo valor fora de faixa mas acima de -100, que é o filtro do firmware (`temperature.cpp:86`) |
| `wifi.rssi` | -120 a 0 | driver devolvendo 0 ou positivo em transição |
| `device_id` | slug minúsculo | NVS corrompida |
| `rtc.time` | `HH:MM` | RTC devolvendo hora inválida |

Em todos, o padrão é o mesmo e é o inverso do que o sistema deveria fazer: **o
relatório é descartado porque um componente com defeito relatou um número
estranho.** Um sistema de observabilidade tem que ser mais robusto que a coisa
que ele observa.

### A correção

Três camadas, e vale fazer as três:

**1. Aceitação degradada no servidor.** Separar o payload em núcleo e periferia.
O núcleo — `device_id`, `boot_id`, `seq` — precisa ser válido; sem ele não há o
que fazer. Todo o resto deve ser validado **por bloco**, com o bloco inválido
virando `null` mais um evento de servidor (`ingest.field_rejected`, com o campo e
o valor recebido), em vez de derrubar o POST.

Isso transforma o incidente atual em: "a ventoinha reportou 70.000 RPM, que é
impossível — o tacômetro está com problema". Que é exatamente o diagnóstico
correto, entregue automaticamente.

**2. Saneamento no firmware.** `rpm` deve ser limitado antes de sair
(`constrain(_rpm, 0, 20000)`), e a rejeição registrada como
`fan.tach_implausible`. O firmware sabe que 70.000 RPM é absurdo; ele não deveria
depender do servidor para descobrir.

**3. O 400 precisa parar de ser tratado como erro transitório.** Hoje o firmware
reage a 400 igual a um timeout: recua e repete. Mas 400 é determinístico — repetir
o mesmo corpo dá o mesmo resultado. O firmware deveria registrar
`api.rejected_body`, **avançar o `seq`** para não ficar preso ao corpo
envenenado, e seguir. Sem isso, mesmo com as correções acima, um campo novo
incompatível trava o dispositivo até alguém ir lá.

---

## C2 — O `.h` do firmware nunca foi gerado a partir do contrato

**Alta.** `docs/plano-migracao-bettacare.md` registra a promessa:

> O corpo do POST é definido **uma vez**, em Zod. O servidor valida com ele, o
> front tipa com ele, e o `.h` do firmware é **gerado** a partir dele por um
> script.

Não existe script de geração. Não há nada em `packages/contract` nem em
`apps/server` que emita cabeçalho C, e `firmware/bettacare/` não tem arquivo
gerado. **O firmware reimplementa o contrato à mão**, em `api_client.cpp:64-158`
para serializar e `event_log.cpp:13-18` para os nomes de enum.

Ou seja: das três pontas, duas compartilham a fonte de verdade (servidor e front,
ambos em TypeScript) e a terceira — justamente a que é difícil de atualizar,
porque exige gravar um dispositivo físico — está fora.

O documento diz que essa era "a classe de bug mais chata deste projeto". Ela não
foi eliminada, só reduzida de três cópias para duas.

**As divergências que isso já produziu** estão listadas em C3 e C4. Nenhuma é
catastrófica hoje, o que é sorte e disciplina, não garantia.

**Correção, em ordem de custo:**

- **Barato e imediato:** um teste que carregue os nomes de enum do firmware
  (`event_log.cpp:13-18`, `app_state.cpp:20-23`) por extração de texto e os
  compare com `COMPONENTS`, `SEVERITIES` e os enums do contrato. Não gera nada,
  mas quebra o build quando divergirem.
- **Correto:** um script `pnpm gen:firmware-header` que emita
  `firmware/bettacare/contract_generated.h` com os enums, os limites numéricos e
  os tamanhos de buffer. Rodado na CI com verificação de que o arquivo
  versionado está em dia.

O segundo é meio dia de trabalho e fecha a promessa original.

---

## C3 — Metade do que o firmware diz chega ao app sem tradução

**Média.** O catálogo em `codes.ts` traduz códigos para português e sugere
próximos passos. Um código ausente não é erro — `primitives.ts:71-77` explica
corretamente que o evento é gravado igual e exibido com o rótulo cru.

O problema é o tamanho da lacuna. Cruzando o que o firmware emite com o que o
catálogo conhece:

**Emitidos pelo firmware e ausentes do catálogo (22 códigos):**

`system.ready`, `system.reboot_requested`, `system.task_failed`,
`system.config_loaded`, `system.config_applied`, `nvs.boot_id_failed`,
`nvs.open_failed`, `nvs.no_config`, `nvs.write_failed`, `api.connected`,
`api.bad_config`, `api.unknown_command`, `api.queue_full`, `api.server_error`,
`rtc.found`, `rtc.ntp_timeout`, `fan.pwm_failed`, `fan.tach_ok`,
`fan.mode_auto`, `fan.mode_manual`, `fan.failsafe_cleared`, `fan.escalated`,
`wifi.ap_mode_off`.

Alguns são graves e aparecem crus na aba Logs justamente na hora ruim:
`fan.pwm_failed` é `SEV_FATAL` e significa que a ventoinha não tem controle
nenhum. `system.task_failed` significa que o dispositivo está operando sem rede
por decisão própria. `nvs.write_failed` significa que a configuração não vai
sobreviver ao próximo reboot.

**No catálogo e emitidos por ninguém (9 códigos):**

`system.panic`, `system.low_memory`, `light.gpio_fault`, `pot.out_of_range`,
`temp.out_of_range`, `button.pressed`, `cmd.device_offline`, `cmd.acked`,
`cmd.queued`.

Vale ler essa lista com atenção, porque ela é um mapa das promessas não
cumpridas: `system.panic` e `system.low_memory` são o F7 do firmware
(instrumentação ausente), `pot.out_of_range` é o S2 do servidor (saúde do
potenciômetro), `cmd.device_offline` é o S4 (comando que expira em `queued`), e
`light.gpio_fault` é o F10 (falha do SSR estruturalmente indetectável). O
catálogo foi escrito a partir do desenho; o desenho não chegou inteiro ao código.

**Um erro de nome, não de ausência:** o catálogo tem `api.server_unreachable`; o
firmware emite `api.server_error` (`api_client.cpp:319`). São o mesmo conceito
com dois nomes. Um dos dois precisa ceder.

**Correção:** acrescentar os 22 ao catálogo com rótulo e, onde houver, `hint`.
É trabalho mecânico de uma hora e melhora a aba Logs mais que qualquer outra
mudança de front. Marcar os 9 órfãos com um comentário dizendo qual etapa vai
passar a emiti-los — ou removê-los, se a resposta for "nenhuma".

---

## C4 — Divergências campo a campo

Comparação do que o firmware serializa, do que o Zod aceita e do que o servidor
lê. Só as linhas com problema; o resto do payload confere.

| Campo | Firmware envia | Contrato aceita | Servidor lê | Veredito |
|---|---|---|---|---|
| `events[].t` | **nunca envia** — manda `ctx.device_time` como `"HH:MM"` (`api_client.cpp:153`) | `isoInstantSchema.optional()` | `e.t` (`process.ts:326`) | **Divergente.** `events.device_time` é sempre nulo; o dado real fica enterrado no JSONB. Ver S7 |
| `wifi.rssi` | `-120` como sentinela de desconectado (`wifi_manager.cpp:285`) | `.min(-120)` — **aceita** | trata como medição; `< -80` vira `degraded` | **Divergente.** Sentinela viajando como valor — a família do bug de `a4596fe` |
| `fan.rpm` | `uint16_t` truncado, sem limite superior | `.max(20000)` | — | **Divergente.** Ver C1 |
| `health` | **nunca envia** | `deviceHealthReportSchema.optional()` | não lê | **Órfão.** Bloco inteiro do contrato sem uso nas duas pontas |
| `api_failures` | existe (`api_client.cpp:59`), **não serializa** | não tem campo | — | **Ausente.** Impede a regra de saúde de `api` (S3) |
| `button`, `pot` | nenhum campo | nenhum campo | não avalia | **Ausente.** Dois componentes do desenho sem canal (F10, S2) |
| `diag` (heap, reset) | nada | nada | nada | **Ausente.** O centro do pedido desta rodada (F7, S1) |

Confere e não precisa mexer: `device_id`, `boot_id` (o `esp_random() &
0x7FFFFFFF` do firmware casa exatamente com `INT32_MAX`), `seq`, `uptime_ms`,
`config_version`, `fw_version`, `light.*`, `temperature.*` (incluindo o
`age_ms` nulo, que é a correção de `a4596fe` bem feita), `fan.on/speed/mode`,
`rtc.*`, `wifi.ip/reconnects`, `ack[]` no formato `{id, ok, code?}`, e os
tamanhos de `code` (40 no firmware, 48 no contrato) e `msg` (120 no firmware,
240 no contrato) — o firmware é mais estrito nos dois, então não há estouro.

---

## C5 — Versionamento: campo novo desaparece em silêncio

**Média.** `telemetryRequestSchema` usa `z.object()`, que no Zod **remove**
chaves desconhecidas em vez de rejeitá-las.

Para compatibilidade futura isso é a escolha certa: um firmware novo falando com
um servidor velho não quebra. Mas a consequência precisa estar clara antes da
Etapa 1 das outras frentes: se o firmware começar a mandar o bloco `diag` e o
servidor ainda não o conhecer, **o POST é aceito, responde 200, e o `diag` é
descartado sem nenhum aviso**. Tudo parece funcionar; o dado simplesmente não
existe.

**Correção:** um contador de chaves desconhecidas no ingest, registrado como
evento de servidor uma vez por versão de firmware vista. Barato, e transforma
"não funcionou e ninguém sabe por quê" em uma linha no log.

E a regra de ordem que decorre disso, válida para todo o `UPGRADE/`:
**contrato → servidor → firmware.** Nunca o contrário.

---

## C6 — Fronteira e segurança

Verificado e correto:

- `ingest/auth.ts` — SHA-256 dos dois lados antes do `timingSafeEqual`, que
  resolve o vazamento de comprimento. É o melhor arquivo do repositório.
- **Isolamento das duas portas** — `ingest/router.ts:13-22` não registra o
  middleware do Cloudflare Access, então o cabeçalho `Cf-Access-...` é ignorado
  incondicionalmente na porta 8080. Correto: aquele tráfego não passa pelo
  Access, e confiar no cabeçalho ali seria confiar em qualquer um da LAN.
- `app.all("*")` devolvendo 404 na porta do ESP32.

Pontos em aberto:

- **HTTP puro na LAN.** Decisão consciente e registrada em `config.example.h:47`
  ("é tráfego local e o custo de TLS num ESP32 não se paga aqui"). Concordo. A
  consequência a aceitar de olhos abertos: quem estiver na LAN vê o token de
  ingestão em texto claro. O token dá acesso a **escrever telemetria**, não a
  controlar o aquário — comandos só saem pela porta 3000, atrás do Access.
  O pior caso é poluição de dados, não perda de controle. Vale registrar por
  escrito para a decisão não ser reaberta sem contexto.
- **Sem limite de tamanho e sem limite de taxa** no ingest — ver S11.
- **A porta 3000 não pode vazar.** Toda a autenticação de humanos depende de um
  cabeçalho HTTP que o Cloudflare Access injeta. Se a 3000 ficasse alcançável
  fora do túnel, qualquer um forjaria o cabeçalho e teria controle total. O
  compose não publica a porta, o que está correto — mas é uma propriedade que
  precisa ser **verificada de fora** depois de cada mudança de infraestrutura,
  não presumida.

---

## Plano de ação

### Etapa 1 — Tolerância a dado ruim (C1)

**Faça antes de qualquer outra coisa em qualquer frente.** É o que impede o
próximo defeito de hardware de apagar o próprio rastro.

1. Servidor: validação por bloco, com bloco inválido virando `null` e evento
   `ingest.field_rejected`.
2. Firmware: limitar `rpm` na origem e emitir `fan.tach_implausible`.
3. Firmware: tratar `400` como determinístico — registrar, avançar o `seq`,
   seguir.

**Critério de aceite:** injetar `"rpm": 99999` num POST simulado com `curl`; o
servidor responde `200`, grava o resto do estado, e a aba Logs mostra o campo
rejeitado com o valor recebido.

### Etapa 2 — Bloco `diag` no contrato (C4, C5)

Pré-requisito da Etapa 1 do firmware e da do servidor.

1. `diagSchema` opcional, com todos os campos do F7 e tetos casando com as
   colunas da migration `0004`.
2. Contador de chaves desconhecidas no ingest.
3. Remover ou implementar `deviceHealthReportSchema`, hoje órfão.

### Etapa 3 — Catálogo completo (C3)

1. Os 22 códigos que faltam, com rótulo e `hint`.
2. Resolver `api.server_error` vs `api.server_unreachable`.
3. Comentar os 9 órfãos com a etapa que vai emiti-los.

### Etapa 4 — Fechar a promessa da fonte única (C2)

1. Começar pelo teste de consistência de enums — barato, e já pega a próxima
   divergência.
2. Depois o gerador de `contract_generated.h`, verificado na CI.

### Etapa 5 — Corrigir as divergências residuais (C4)

1. `wifi.rssi` nulo quando desconectado, nas duas pontas.
2. Decidir o lado do `device_time` e implementar.

---

## Dependências com as outras frentes

- **C1 é a raiz de um caminho que atravessa as três camadas.** O
  [documento 02](02-hardware-e-ligacoes.md) §4 descreve a causa física, este
  documento o mecanismo, e o [plano de servidor](04-plano-servidor-e-banco.md)
  S12 o falso positivo relacionado. Corrigir só o resistor resolve o incidente;
  corrigir só o contrato resolve a classe.
- **C5 fixa a ordem de execução** de tudo: contrato, depois servidor, depois
  firmware.
- A ordem contrária — firmware primeiro — **não dá erro**, e é exatamente por
  isso que é perigosa.
