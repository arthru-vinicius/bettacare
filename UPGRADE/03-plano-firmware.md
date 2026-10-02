# Firmware — achados e plano de ação

Auditoria completa de `firmware/bettacare/` (25 arquivos, ~3.100 linhas), lida
inteira, arquivo por arquivo.

**Veredito geral:** o firmware é bom. A separação em dois núcleos está
corretamente implementada, o mutex do I²C com cache de minutos evita um deadlock
real e não óbvio, a máquina de estados da ventoinha é madura, e a lição do 500
causado por sentinela (commit `a4596fe`) foi aplicada onde importava. Isto não é
código para reescrever.

Mas há **quatro bugs que produzem exatamente os sintomas que motivaram a
rodada**, e uma lacuna de instrumentação que impede diagnosticar o resto.

---

## Sumário dos achados

| # | Achado | Onde | Gravidade |
|---|---|---|---|
| F1 | Eventos e `ack` são drenados **antes** do POST e perdidos se ele falhar | `api_client.cpp:255,258` | **Crítica** |
| F2 | `device.reboot` reinicia antes de o `ack` ser enviado | `bettacare.ino:78` | **Alta** |
| F3 | NTP nunca ressincroniza depois do primeiro sucesso | `net_task.cpp:36` | **Alta** |
| F4 | `temp.sensor_lost` nunca é emitido no caminho de falha por CRC | `temperature.cpp:99` | **Alta** |
| F5 | Botão amostrado a 5 Hz — toques curtos são perdidos | `bettacare.ino:33,156` | **Alta** |
| F6 | `wifi_manager` sem sincronização, tocado por duas tasks | `wifi_manager.cpp` | **Média** |
| F7 | Sem `free_heap`, `min_free_heap` nem `reset_reason` na telemetria | `app_state.h:43` | **Alta** (lacuna) |
| F8 | Alocação de `String` no laço quente — fragmentação em semanas | `bettacare.ino:109,114` | **Média** |
| F9 | Dedup de eventos só compara com a última entrada | `event_log.cpp:53` | **Média** |
| F10 | Saúde de `light`, `button` e `pot` é estruturalmente incomputável | vários | **Média** |
| F11 | Sem task watchdog — task de rede travada não é recuperada | — | **Média** |
| F12 | `comp=pot` emitindo `code=fan.*` | `fan.cpp:243,252` | **Baixa** |
| F13 | `wifi_rssi()` devolve `-120` como sentinela em vez de nulo | `wifi_manager.cpp:285` | **Baixa** |
| F14 | Token da API local comparado sem tempo constante | `web_server.cpp:71` | **Baixa** |

---

## Os achados em detalhe

### F1 — Eventos e confirmações são perdidos quando o POST falha

**Crítica.** É o achado mais grave do firmware, e ele contradiz a própria
documentação do módulo.

`event_log.h:78-82` promete, textualmente:

> Só a task de rede chama isto, e só depois de o POST ter sido aceito — se
> drenasse antes, uma falha de rede perderia os eventos justamente no momento em
> que eles importam.

O código faz o contrário. Em `api_client.cpp`:

```
255   uint8_t n_eventos = event_log_drain(eventos, EVENT_BUFFER_SIZE);
258   uint8_t n_acks    = app_state_drain_acks(acks, 8);
...
283   int status = http.POST(corpo);
```

A drenagem acontece **28 linhas antes** do POST. Se a requisição falhar, os
eventos e os `ack` já saíram do buffer e não voltam.

Para os **eventos**, o comentário em `api_client.cpp:310-316` argumenta que a
perda é deliberada — reinserir criaria um laço em que falha de rede gera evento
que engorda o corpo que tem mais chance de falhar. O argumento é bom e a decisão
é defensável. **O problema é que o comentário só fala de eventos, e a mesma
linha de código descarta os `ack`.**

E `ack` perdido é outra coisa inteiramente. A consequência é direta e visível
para você:

1. Você toca em "Ligar luminária" no app.
2. O servidor enfileira o comando; o dispositivo o recebe e **executa** — a luz
   acende.
3. O firmware empilha o `ack` (`bettacare.ino:51`).
4. O POST seguinte falha (Wi-Fi oscilou, servidor reiniciando).
5. **O `ack` desapareceu.** Não há como reenviá-lo.
6. Noventa segundos depois o watchdog do servidor marca o comando como
   `expired` e a interface exibe *"Comando expirou sem confirmação"*.

A luz está acesa e o app diz que falhou. É precisamente a classe de confusão que
a seção 4 de `docs/arquitetura-observabilidade.md` foi escrita para eliminar.

**Correção:** separar os dois destinos. Eventos podem continuar sendo
descartados na falha — a justificativa se sustenta. `ack` **precisa** sobreviver:
drená-lo apenas depois de `status == 200`, ou mantê-lo na fila e removê-lo por
confirmação do servidor. A fila de `ack` tem 8 posições e comandos são raros;
não há risco de inundação.

### F2 — `device.reboot` reinicia antes de confirmar

**Alta.** Em `bettacare.ino:72-80`:

```
75      app_state_push_ack(cmd.id, true, nullptr);
78      delay(1500);   // dá tempo de o próximo POST levar o ack
79      ESP.restart();
```

O comentário revela a intenção certa e a implementação errada. O `ack` entra
numa fila que só é drenada pelo **próximo POST**, e o intervalo padrão de
telemetria é de **3.000 ms** (`config.example.h:66`) — o dobro da espera. Com o
recuo progressivo ativo (`net_task.cpp:65-71`), o próximo POST pode estar a
**até 30 segundos**.

Ou seja: no caso comum o dispositivo reinicia antes de o `ack` sair, e
`device.reboot` aparece como falha mesmo tendo funcionado. Somado ao F1, o
comando praticamente nunca é confirmado.

Há ainda um problema menor de invariante: `delay(1500)` roda no laço de
controle, que o cabeçalho do arquivo declara que "nunca bloqueia". Por 1,5 s
antes de um reboot é irrelevante na prática, mas quebra a regra.

**Correção:** não reiniciar por temporizador. Marcar um pedido pendente, deixar
a task de rede reiniciar **depois** de um POST bem-sucedido que tenha levado o
`ack`. Um teto de segurança (reiniciar de qualquer forma após ~60 s) evita ficar
preso se a rede não voltar.

### F3 — O relógio nunca é ressincronizado

**Alta.** `net_task.cpp:36`:

```
36    if (!_ntp_synced && wifi_is_connected()) {
```

`_ntp_synced` vira `true` no primeiro sucesso e **nunca mais volta a `false`**.
A partir daí `rtc_sync_ntp()` não é chamada outra vez enquanto o dispositivo não
reiniciar.

Enquanto o DS3231 estiver bem, isso não incomoda — ele é preciso. O problema é o
caso que a rodada existe para cobrir: **a bateria acabar em operação.** Quando
isso acontece, o DS3231 perde a hora, `rtc_lost_power()` passa a ser verdadeiro,
e a hora fica errada **para sempre** — porque a única função que a corrige
(`rtc_manager.cpp:152`, `_rtc.adjust(...)`) nunca mais será chamada.

Uma hora errada quebra a automação da luminária inteira: a janela horária passa
a ser avaliada contra um relógio sem sentido. O aquário fica com a luz acesa de
madrugada e apagada de dia, e nada no sistema explica por quê.

**Correção:** ressincronizar quando `rtc_lost_power()` ficar verdadeiro, e
periodicamente (uma vez por dia basta, e é barato). Emitir evento na correção,
para que a transição fique registrada.

### F4 — O sensor de temperatura some sem dizer que sumiu

**Alta.** Em `temperature.cpp:94-101`, depois de três leituras inválidas
seguidas:

```
96      event_log(SEV_WARN, COMP_TEMP, "temp.crc_error", ...);
99      _available = false;   // força a re-sondagem, que emitirá temp.sensor_lost
```

O comentário está errado sobre o próprio comportamento. `_probe()` começa assim
(`temperature.cpp:41`):

```
41    if (encontrado == _available) return;   // sem transição, nada a dizer
```

Como a linha 99 já colocou `_available = false`, quando `_probe()` roda e o
sensor de fato não está lá, temos `encontrado == false` e `_available == false`
— **igual, retorna cedo, nenhum evento.**

Resultado: um DS18B20 que morre por cabo solto emite **um** `temp.crc_error` e
depois silêncio absoluto. O evento `temp.sensor_lost`, que é o que responde à
pergunta *"desde quando o sensor de temperatura sumiu?"* — pergunta nº 2 das
três que guiaram todo o desenho de observabilidade — nunca é emitido nesse
caminho.

**Correção:** emitir a transição explicitamente no ponto onde ela é decidida
(linha 99), em vez de delegar a `_probe()`. Ou usar uma flag separada de
"suspeito" para que `_probe()` enxergue a transição real.

### F5 — O botão físico perde toques curtos

**Alta**, e é uma das queixas originais.

`bettacare.ino:33` fixa `LOOP_PERIOD_MS = 200`, e `light_check_button()` é
chamada uma vez por ciclo (`bettacare.ino:156`). O botão é lido por **polling a
5 Hz**.

O comentário na linha 32 diz que "200 ms é imperceptível num botão físico". Isso
está certo sobre **latência** e errado sobre **detecção**. Para o toque ser
visto, o pino precisa estar em nível baixo no instante exato da amostragem. Um
toque rápido — 80 a 150 ms, que é o normal — cabe inteiro entre duas amostras e
**não é visto de jeito nenhum**.

O comportamento resultante é "às vezes o botão não funciona", intermitente e
irreproduzível, sem nada no log, porque do ponto de vista do firmware não houve
toque nenhum.

**Correção:** interrupção no GPIO18 marcando uma flag, com o debounce de 50 ms
continuando no laço. Custa poucas linhas e resolve por completo. Alternativa mais
barata: ler o botão num ciclo próprio de 20 ms, mas a interrupção é mais correta
e não come tempo de laço.

> **Superado em 2026-10-01.** A interrupção resolveu o toque perdido e criou
> um defeito pior: ela aceitava como toque qualquer borda, inclusive os picos
> de microssegundos que o 1-Wire do GPIO19 induz no fio do botão, e o
> "debounce de 50 ms no laço" não filtrava nada com o laço a 200 ms. Era o relé
> trocando sozinho a cada ~5 s. A versão atual faz a alternativa de "ciclo
> próprio", só que mais curta e fora do laço: um `esp_timer` amostra o nível a
> cada 2 ms e só aceita 30 ms estáveis. Ver o fim do
> [07](07-roteiro-de-execucao.md#rodada-de-bancada--2026-10-01).

### F6 — `wifi_manager` é tocado por duas tasks sem nenhuma proteção

**Média.** `wifi_manager.cpp` mantém estado em objetos `String`
(`_configured_ssid`, `_configured_password`, `_recovery_ap_ssid`, linhas 38-40) e
**não tem mutex nenhum**.

Quem toca esse estado:

- a **task de rede**, por `wifi_check_reconnect()` → `_start_connect_attempt()`
  (`net_task.cpp:32`), que lê `_configured_ssid.c_str()` na linha 176;
- a **task do AsyncTCP**, quando alguém salva credenciais pelo portal de
  recuperação: `web_server.cpp` chama `wifi_set_credentials()`, que **atribui**
  às mesmas `String` (linhas 262-263).

Atribuir a uma `String` libera o buffer antigo e aloca outro. Se a task de rede
estiver lendo `c_str()` nesse instante, ela segue um ponteiro liberado. É uma
corrida real, de janela estreita — o tipo que aparece uma vez por mês e é
impossível de reproduzir.

**Correção:** mutex em torno do estado de credenciais, ou trocar as `String` por
buffers `char[]` de tamanho fixo (33 e 64 bytes) preenchidos sob proteção. A
segunda opção resolve também parte do F8.

### F7 — Falta a instrumentação que responderia às perguntas em aberto

**Alta**, e é o coração do que você pediu.

`DeviceSnapshot` (`app_state.h:43-66`) carrega hoje: luz, temperatura,
ventoinha, RTC, RSSI, IP, contador de reconexões e `uptime_ms`. É um bom começo
— RSSI e reconexões já são mais do que a maioria dos projetos tem.

O que **não** existe, e que é justamente o que diagnostica falha intermitente:

| Campo | Por que importa |
|---|---|
| `reset_reason` | **O mais importante.** Distingue brownout (hardware, ver [documento 02](02-hardware-e-ligacoes.md)) de task watchdog (software) de reboot comandado. Sem ele, a discussão sobre alimentação é opinião |
| `free_heap` | Vazamento de memória aparece como queda lenta ao longo de dias |
| `min_free_heap` | O pior momento desde o boot — pega o pico que a amostragem de 3 s nunca veria |
| `max_alloc_heap` | Fragmentação: heap livre alto com maior bloco pequeno é o retrato exato do F8 |
| `post_latency_ms` | Latência do POST anterior. Rede degradando antes de cair |
| `api_consecutive_failures` | Já existe em `api_client.cpp:20`, mas nunca é enviado |
| `tach_pulses_raw` | Contagem crua na janela, além do RPM calculado — separa "não gira" de "tacômetro ruidoso" |
| `pot_raw_adc` | Detecta mau contato e saturação do ADC; hoje `pot` é invisível |
| `button_pressed` | Estado instantâneo, para a saúde de `button` ser computável |
| `heap_min_since_boot` / `stack_hwm` | Marca d'água da pilha das duas tasks |

`uptime_ms` já existe, mas é `uint32_t` vindo de `millis()` — **estoura em 49,7
dias**. O servidor precisa tratar o retorno a zero como continuidade, não como
reboot. Vale registrar porque um aquário fica ligado meses.

**Correção:** ampliar `DeviceSnapshot`, preencher em `_publish_snapshot()`
(`bettacare.ino:91`) e serializar num bloco `diag` em `_build_body()`
(`api_client.cpp:64`). O custo no payload é de ~90 bytes; o corpo hoje varia
entre 391 e 777 bytes e o teto de resposta assumido é 300 — folga suficiente.

**Priorize `reset_reason` acima de tudo.** É um campo, custa nada, e é o que
transforma as próximas semanas de depuração em algo dirigido por dado.

### F8 — Alocação de heap no laço quente

**Média.** `_publish_snapshot()` roda a cada 200 ms e chama:

- `rtc_get_time_str()` (`bettacare.ino:109`), que retorna `String(buf)`
  (`rtc_manager.cpp:129`) — uma alocação;
- `wifi_local_ip()` (`bettacare.ino:114`), que retorna
  `WiFi.localIP().toString()` (`wifi_manager.cpp:289`) — outra.

São **10 alocações por segundo**, 864 mil por dia. Some-se `event_log.cpp:79`,
que faz `String t = rtc_get_time_str();` **segurando o mutex do log**, e
`api_client.cpp:82`, que constrói uma `String` temporária por POST.

Cada alocação é pequena e liberada logo. O problema não é vazamento, é
**fragmentação**: o heap do ESP32 vai ficando picado, o maior bloco contíguo
encolhe, e um dia uma alocação grande — o buffer do POST, o handshake — falha
num sistema que ainda mostra bastante "heap livre". É o retrato clássico de
"funciona por semanas e depois começa a dar problema".

**Correção:** trocar por preenchimento de buffer do chamador
(`rtc_format_time(char* out, size_t n)`), que é uma mudança mecânica e local. O
campo `max_alloc_heap` do F7 é o que confirma se o problema é real nesta
montagem — meça antes de otimizar.

### F9 — A deduplicação de eventos é mais fraca do que o documento promete

**Média.** `event_log.cpp:53-59` compara o evento novo apenas com `_buf[_count-1]`
— a **última** entrada. O cabeçalho é honesto sobre isso ("enquanto ainda for a
entrada mais recente"), mas `docs/arquitetura-observabilidade.md` §2 promete algo
mais forte: *"Um sensor oscilando não pode inundar o banco."*

Um componente oscilando raramente repete o mesmo código em sequência — ele
**alterna**. `temp.sensor_lost`, `temp.sensor_found`, `temp.sensor_lost`… Cada
par quebra a dedup, e o buffer de 24 posições enche em segundos. Aí entra o
descarte por estouro (`event_log.cpp:62-68`), que joga fora **os mais antigos** —
ou seja, o começo do episódio, que é a parte que explica o que aconteceu.

**Correção:** dedup por varredura das últimas N entradas (N = 4 ou 8 resolve o
caso de alternância) em vez de só a última. O custo é uma comparação de string
curta num buffer minúsculo.

### F10 — Três componentes têm saúde estruturalmente incomputável

**Média**, e este achado é conceitual: a implementação não "esqueceu" desses
casos, ela não tem como atendê-los com o desenho atual.

**Luminária.** `light.cpp:23-27` faz `digitalWrite()` e em seguida
`_state = on`, incondicionalmente. O estado reportado é, por construção, sempre
igual ao comandado. A regra do documento — "estado desejado ≠ estado reportado
após 2 ciclos" — **nunca pode disparar por defeito de hardware**, porque o
firmware acredita nele mesmo.

O que a regra *realmente* detecta é conflito entre fontes de controle: o
servidor manda acender às 18:00 e a automação apaga no mesmo minuto. Isso tem
valor, mas é outra coisa. Vale corrigir a redação do documento de
observabilidade para não prometer um diagnóstico de hardware que ele não faz.

Detectar SSR morto de verdade exige realimentação física — um LDR olhando para a
lâmpada, ou um sensor de corrente não invasivo na fase. É decisão sua se vale;
até lá, o honesto é declarar `light` como **não observável**.

**Botão.** `light_button_stuck()` existe (`light.cpp:39`) e o evento
`button.stuck` é emitido, mas `DeviceSnapshot` não tem campo nenhum de botão —
então o servidor não consegue calcular estado de saúde, só reagir a evento. A
tabela do documento lista `button` como componente monitorado.

**Potenciômetro.** `COMP_POT` existe no enum e é usado em duas linhas
(`fan.cpp:243,252`), sempre para relatar mudança de velocidade. **Nenhum evento
descreve saúde do potenciômetro.** O documento promete `degraded` quando o "pot
está fora da faixa de ADC esperada"; não há código que avalie isso. Um
potenciômetro com mau contato continua invisível — que é literalmente o problema
citado na motivação do documento original.

**Correção:** enviar `button_pressed` e `pot_raw_adc` no bloco `diag` (F7) e
deixar o servidor julgar. Para a luminária, corrigir a promessa no documento.

### F11 — Sem watchdog de task

**Média.** Não há nenhuma chamada a `esp_task_wdt_add` no firmware.

O desenho de dois núcleos é uma proteção real: se a task de rede travar, o
aquário continua funcionando — luz por horário, ventoinha por temperatura, botão
físico. Isso é bom e foi projetado assim de propósito.

Mas travar significa **telemetria morta em silêncio**. O servidor marca o
dispositivo como offline, todos os componentes viram `unknown`, e você conclui
que o ESP32 caiu — quando na verdade ele está lá, controlando o aquário
perfeitamente, com uma única task presa dentro do `HTTPClient`.

**Correção:** inscrever a task de rede no task watchdog com um prazo generoso
(60 s cobre o timeout de 4 s com folga larga). Reinício por watchdog é recuperação
barata, e com `reset_reason` (F7) ele fica **registrado** em vez de misterioso.

### F12 a F14 — Achados menores

**F12** — `fan.cpp:243,252` emitem `comp = pot` com `code = fan.off` / `fan.on`.
Componente e código de famílias diferentes. Quem filtrar por componente `pot`
verá códigos `fan.*`, e quem agregar por código somará ações do potenciômetro
com ações da ventoinha. Renomear para `pot.fan_off` / `pot.fan_speed` e
acrescentar ao catálogo do contrato.

**F13** — `wifi_manager.cpp:285` devolve `-120` quando desconectado, e
`api_client.cpp:113` envia esse número como se fosse medição. A regra de saúde do
documento marca `degraded` abaixo de -80 dBm, então um dispositivo desconectado é
classificado como "Wi-Fi fraco". É a mesma família do bug corrigido em `a4596fe`:
sentinela viajando como valor. Enviar `null`.

**F14** — `web_server.cpp:71` usa `provided.equals(API_AUTH_TOKEN)`, comparação
que retorna cedo na primeira diferença. O servidor faz questão de comparar em
tempo constante com SHA-256; a API local do dispositivo não. Numa LAN doméstica a
severidade é baixa, mas a assimetria não tem motivo.

---

## O que está certo e não deve ser mexido

Registrado para ninguém "corrigir" numa próxima passada:

- **A separação em dois núcleos.** `net_task.cpp` faz exatamente o que promete, e
  o aquário sobrevive sem servidor. Era o defeito estrutural do firmware antigo e
  está resolvido.
- **O cache de minutos do RTC** (`rtc_manager.cpp:38-49`). `rtc_get_time_str()` é
  chamada de dentro de `event_log()`, ou seja, dos dois núcleos. Se tocasse I²C,
  a ordem entre o mutex do log e o do barramento criaria deadlock. O cache
  `volatile uint16_t` elimina a classe inteira. É a melhor decisão do arquivo.
- **Registrar eventos só depois de soltar o mutex do I²C**
  (`rtc_manager.cpp:99-111`). Mesma razão, e é sutil o bastante para alguém
  "simplificar" sem perceber.
- **A máquina de estados da ventoinha** (`fan.cpp:280-353`): histerese, cooldown,
  escalonamento progressivo e failsafe. Portada do sistema antigo e continua
  sendo a lógica mais madura do projeto.
- **`_period_should_be_on()`** (`rtc_manager.cpp:70-74`) trata corretamente a
  janela que cruza a meia-noite.
- **Agir só na transição de período** (`rtc_manager.cpp:192`). É o que faz um
  override manual sobreviver dentro da janela.
- **Comparação de tempo à prova de estouro** em `net_task.cpp:45`.
- **`temp["celsius"] = nullptr`** em vez de sentinela (`api_client.cpp:81-95`).
  A lição do commit `a4596fe` aplicada corretamente. O F13 é o mesmo bug num
  campo que passou batido.
- **Validação da configuração** (`device_config.cpp:31-42`), incluindo a guarda
  `fan_off_c >= fan_trigger_c` que evita oscilação infinita.
- **Recuo progressivo** (`net_task.cpp:54-73`) com teto de 30 s.
- **A leitura da config por cópia sob mutex** (`device_config.cpp:74-79`), que
  evita aplicar metade da configuração nova com metade da velha.

---

## Plano de ação

Ordenado por retorno sobre esforço. As três primeiras etapas cabem numa sessão.

### Etapa 1 — Instrumentação primeiro (F7, F13)

Antes de corrigir qualquer coisa, **instrumente**. É o que transforma as
hipóteses do [documento 02](02-hardware-e-ligacoes.md) em medição, e o que diz
se os outros achados importam nesta montagem.

1. Ampliar `DeviceSnapshot` (`app_state.h:43`) com o bloco de diagnóstico do F7.
2. Preencher em `_publish_snapshot()` (`bettacare.ino:91`).
3. Serializar num objeto `diag` em `_build_body()` (`api_client.cpp:64`).
4. Trocar a sentinela `-120` do RSSI por `null` (F13).
5. Estender o contrato Zod e o schema do banco — ver
   [documento 04](04-plano-servidor-e-banco.md).

**Critério de aceite:** depois de 48 h ligado, o banco responde "quantas vezes o
dispositivo reiniciou, e por quê" e "o maior bloco de heap encolheu ao longo do
tempo?".

### Etapa 2 — Os quatro bugs de comportamento (F1, F2, F4, F5)

São independentes entre si e todos localizados.

1. **F1** — drenar `ack` só após `status == 200`. Manter o descarte de eventos.
2. **F2** — reboot pendente confirmado pela task de rede, com teto de 60 s.
3. **F4** — emitir `temp.sensor_lost` no ponto de decisão (`temperature.cpp:99`).
4. **F5** — interrupção no botão, debounce mantido no laço.

**Critério de aceite:** com o servidor derrubado no meio de um comando, a
interface mostra o desfecho correto quando ele volta. Toques rápidos no botão
acendem a luz **sempre**.

### Etapa 3 — Robustez de longo prazo (F3, F6, F11)

1. **F3** — ressincronizar NTP diariamente e ao detectar `rtc_lost_power()`.
2. **F6** — proteger o estado de credenciais do `wifi_manager`.
3. **F11** — task de rede no task watchdog, prazo de 60 s.

**Critério de aceite:** o cenário "servidor fora do ar por dois dias", que
`docs/firmware.md` lista como não verificado, roda até o fim sem intervenção — e
o relógio continua certo depois.

### Etapa 4 — Qualidade do diagnóstico (F9, F10, F12)

1. **F9** — dedup varrendo as últimas 4 entradas.
2. **F10** — enviar `button_pressed` e `pot_raw_adc`; corrigir a promessa sobre
   a luminária em `docs/arquitetura-observabilidade.md`.
3. **F12** — renomear os códigos de `pot` e atualizar o catálogo do contrato.

### Etapa 5 — Higiene (F8, F14)

1. **F8** — eliminar `String` do laço quente. **Faça depois da Etapa 1**, com
   `max_alloc_heap` em mãos: se a fragmentação não estiver acontecendo nesta
   montagem, é otimização sem causa.
2. **F14** — comparação em tempo constante no token da API local.

---

## Dependências com as outras frentes

- A **Etapa 1 depende do contrato e do banco** ([documento 04](04-plano-servidor-e-banco.md)):
  campo novo no firmware sem campo correspondente no Zod é rejeitado no ingest.
  Contrato primeiro, firmware depois.
- A correção do PWM da ventoinha ([documento 02](02-hardware-e-ligacoes.md), §3)
  pode exigir **inverter o duty** em `_apply_speed()` (`fan.cpp:69`), dependendo
  da medição de bancada. Não mexa nesse arquivo antes de medir.
- O diagnóstico `fan.tach_stalled` (`fan.cpp:195`) tem **falso positivo
  estrutural** se a ventoinha for Tipo B da norma Intel e o duty mínimo dela for
  maior que `FAN_SPEED_LOW` (30 %): a ventoinha desliga sozinha, o firmware
  acusa defeito. Confirme o tipo na bancada antes de confiar no alarme.
- O **flash está em 85 %** (`docs/firmware.md`). Há folga para OTA, mas pouca
  para crescer. As mudanças aqui são pequenas; ainda assim, meça depois da
  Etapa 1.
