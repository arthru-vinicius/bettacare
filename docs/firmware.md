# Firmware do ESP32

Referência da Fase 3. Complementa `arquitetura-observabilidade.md` e
`api-servidor.md`.

---

## Os dois núcleos

A decisão estrutural desta reescrita, e ela corrige um defeito real:

```
núcleo 1 — loop(), a cada 200 ms   núcleo 0 — net_task
├─ ação do botão físico            ├─ Wi-Fi (associação, reconexão, AP de recuperação)
├─ DS18B20                         ├─ POST /api/v1/telemetry
├─ ventoinha (histerese)           ├─ NTP
├─ automação por horário           └─ OTA / portal de recuperação
├─ enlace UART2 com o alimentador
└─ executa comandos                núcleo 0 — timer `btn_sample`, a cada 2 ms
                                   └─ amostra o nível do botão (debounce)
```

No firmware antigo tudo isso vivia num `loop()` só, com `delay(200)`, e o
handshake TLS do MQTT bloqueava por até 6 segundos. Ou seja: **uma
instabilidade de rede travava o botão físico e a histerese da ventoinha ao
mesmo tempo**. Trocar MQTT por HTTP não resolveria — HTTP bloqueia igual. O
que resolve é a separação.

Os dois núcleos não compartilham variáveis soltas. Conversam por três canais,
todos em `app_state.h`:

| Canal | Sentido | Proteção |
|---|---|---|
| `DeviceSnapshot` | controle → rede | mutex, cópia |
| fila de comandos | rede → controle | `xQueue` (8 posições) |
| `ack` | controle → rede | vetor de 8 sob mutex, com *peek* + confirmação |

Os `ack` não usam `xQueue` porque uma fila só deixa tirar o item de vez: aqui o
corpo do POST é montado com uma **cópia**, e eles só saem do vetor depois de o
servidor responder `200` (UPGRADE/03, F1). Um `ack` perdido numa queda de rede
faria a interface dizer "comando expirou" para uma ação que funcionou.

E mais dois recursos compartilhados, cada um com o próprio cuidado:

- **`event_log`** — buffer de 24 eventos com mutex, com o mesmo *peek* +
  confirmação. POST aceito remove o que saiu; POST que falhou descarta
  `debug`/`info`/`warn` e retém `error`/`fatal` para a próxima tentativa.
- **`device_config`** — lido **por cópia sob mutex** (`device_config_snapshot()`).
  A struct tem 24 bytes; sem isso o loop poderia ler metade da configuração
  nova e metade da velha, e uma combinação rasgada com
  `fan_off_c >= fan_trigger_c` faria a ventoinha oscilar sem parar.

### O I2C

`rtc_get_time_str()` é chamado de dentro de `event_log()` — ou seja, dos dois
núcleos, a qualquer momento. Se ele fizesse I2C, duas transações simultâneas
travariam o barramento do DS3231, e a ordem entre o mutex do log e o do I2C
criaria um caminho de deadlock.

Por isso a hora corrente vive num `volatile uint16_t` (minutos desde a
meia-noite), lido e escrito em uma instrução. `rtc_get_time_str()` formata a
partir dele e **não toca hardware**. O barramento só é acessado por
`rtc_check_automation()` e pelo autodiagnóstico (núcleo 1) e por
`rtc_sync_ntp()` (núcleo 0), todos sob `_i2c_mutex`, e nenhum registra evento
enquanto o segura.

**A leitura é conferida, não aceita.** O `RTC_DS3231::now()` do RTClib ignora o
retorno da transação I²C e, quando ela falha, decodifica como hora o que havia
na pilha — foi assim que "40:08" apareceu em campo. O firmware lê os
registradores direto e exige transação bem-sucedida e BCD válido em modo 24 h;
leitura ruim é descartada (`rtc.bad_read`) e o cache fica com o último valor
bom. Três falhas seguidas dão o módulo como ausente (`rtc.missing`), e daí em
diante ele é procurado a cada 15 s.

**Sem o DS3231, a hora do ESP32.** Enquanto o módulo está ausente, a automação
segue pelo relógio interno, acertado por NTP e mantido pelo SNTP do ESP-IDF (a
cada 3 h). Antes de qualquer NTP não há hora nenhuma, e a automação espera.

**Virada de período confirmada.** Fora do boot, a luz só troca pelo horário
quando uma segunda leitura do relógio, 10 s depois, concorda com a primeira. Um
byte corrompido que ainda forme hora válida não acende a luz por 10 s — e,
mais importante, não apaga na volta um override manual que o usuário fez.

### Quem vigia quem

O *task watchdog* do ESP-IDF (60 s, com reinício) vigia a **task de rede** e,
desde 2026-10-01, também o **loop de controle** (`enableLoopWDT()` no fim do
`setup()`). Antes só a rede era vigiada: um loop travado — barramento preso,
deadlock — congelaria luz, ventoinha e botão em silêncio, enquanto a telemetria
seguia saindo com o último retrato publicado. O motivo do reinício vai no
`diag.reset_reason` do POST seguinte (`task_wdt`).

O core já sobe o watchdog com 5 s; a task de rede o **reconfigura** para 60 s
(`esp_task_wdt_reconfigure`), porque um `esp_task_wdt_init` em cima do
existente falha — e, enquanto falhava calado, o prazo real era 5 s e um NTP
lento reiniciava o chip.

### O botão

Amostrado por um `esp_timer` a cada 2 ms, fora do loop: a pressão conta depois
de 30 ms seguidos em nível baixo, a soltura depois de 30 ms em nível alto, e o
loop só consome as pressões já confirmadas. A interrupção por borda que existia
antes aceitava como toque os picos que o 1-Wire do `GPIO19` induz no fio do
`GPIO18` — era o relé trocando sozinho a cada ~5 s (UPGRADE/07; números em
`pinagem-e-montagem-esp32.md`, §2).

---

## O que foi portado quase literalmente

- **`fan.cpp`** — histerese, cooldown de 30 min, escalonamento progressivo e
  failsafe de temperatura indisponível. É a lógica mais madura do projeto.
  Mudou de onde vêm os limiares (agora de `device_config`, ou seja, do
  servidor) e, na 2.0.3, o potenciômetro e o desligar — ver abaixo.
- **`rtc_manager.cpp`** — a regra de **agir só na transição de período** está
  intacta. É ela que faz um override manual sobreviver dentro da janela: sem
  isso, apagar a luz às 14h faria a automação reacendê-la no segundo seguinte.
- **`wifi_manager.cpp`** — AP de recuperação e credenciais em NVS.

## O que mudou de verdade

| Antes | Agora |
|---|---|
| `mqtt_manager.*` | `api_client.*` — POST a cada 3 s, comandos na resposta |
| `log_manager` (30 strings em RAM) | `event_log` — sev/comp/code estruturados |
| Config no NVS como fonte de verdade | Servidor manda; NVS vira cache offline |
| — | `boot_id` (NVS) + `seq` monotônico, para idempotência |
| `light.toggle` pela rede | `light.set{on}` — estado desejado |
| Sensor sondado só no boot | Re-sondagem a cada 10 s |
| Tacômetro só reportado | Tacômetro **vigiado** (`fan.tach_stalled`) |
| PWM 0 % para "desligar" | Corte físico do `GND` da ventoinha (`GPIO26`), lido de volta a cada 2 s |
| Qualquer leitura do DS18B20 aceita | 85,0 °C de power-on descartado (`temp.reset_value`); fora de 10–45 °C descartada (`temp.implausible`); salto de mais de 2 °C só com a releitura confirmando (2.0.2) |

Sobre a re-sondagem: antes, um DS18B20 que caísse depois do boot deixava
`available` verdadeiro para sempre e as leituras apenas paravam de mudar. O
requisito de "saber quando um componente deixou de ser reconhecido" não tinha
como ser atendido sem isso.

### O `seq` e o reenvio

O `seq` **só avança quando a troca é bem-sucedida**. Se a resposta se perder, a
tentativa seguinte repete o mesmo `seq`, o servidor reconhece o reenvio pela
guarda `device_state.last_seq` e devolve a mesma lista de comandos.

### Confirmação na hora

Um comando executado não espera o próximo ciclo para ter o `ack` enviado: a
confirmação nova antecipa o POST, que leva junto o estado já mudado. A
geração de `ack` só fica visível para a rede junto com o retrato publicado
**depois** do comando — senão o POST antecipado sairia com "confirmado" e o
estado de antes. No máximo um POST antecipado por leva, e nunca com o servidor
falhando (aí vale o recuo abaixo).

### Recuo progressivo

Com o servidor respondendo erro (5xx, 401), o intervalo cresce até 30 s. Sem
isso seriam requisições a cada segundo sem propósito. Já a falha de rede (sem
resposta) recua no máximo até 10 s: no Wi-Fi do aquário ela passa na tentativa
seguinte, e esperar 30 s só atrasava um comando que estava na fila (2.1.1). O
aquário segue operando sozinho e volta a falar quando houver com quem.

### Wi-Fi ruim (2.1.1)

Instalado no aquário, a 4–5 m do roteador e com uma parede no caminho, o ESP32
passou a ver −63 a −69 dBm de média — com saltos de −84 a −55 em segundos — e
perdeu o contato 103 vezes numa semana (mediana de 2,3 min; o pior, 5,8 h).
O banco mostrou que nos apagões longos o Wi-Fi tinha caído de fato
(`wifi_reconnects` +1 em cada um) e levado horas para voltar. A 2.1.1 ataca
cada elo disso:

- **Rádio** (`wifi_manager.cpp`, `_tune_radio`). Sem economia de energia (o
  rádio não dorme entre beacons); 20 s sem beacon antes de desassociar, em vez
  dos 6 s do ESP-IDF; 20 MHz de banda; e varredura de **todos** os canais,
  escolhendo o ponto de acesso de sinal mais forte. Antes ele entrava no
  primeiro com o nome certo — roteador ou repetidor, o que viesse antes.
- **Ponto de acesso fixado.** Em `/wifi-setup` (login do OTA, pela rede local
  ou pelo AP de recuperação) aparece a lista de pontos de acesso com o nome da
  rede — BSSID, canal e sinal — e dá para fixar um. Fixado e sem conectar por
  5 min, o ESP32 usa o mais forte para não ficar sem rede e procura o fixado de
  novo a cada 30 min. A escolha fica na NVS.
- **Preso sem Wi-Fi** por 10 min, o rádio é desligado e religado
  (`wifi.radio_restart`), de novo a cada 10 min. Não reinicia o ESP32: luz,
  ventoinha e comando manual seguem como estão.
- **HTTP.** A conexão com o servidor fica aberta entre POSTs (keep-alive; o
  servidor segura 75 s). Abrir uma por POST custava sete pacotes, e um SYN
  perdido, 3 s. Prazos de 7 s para conectar e 8 s para a resposta (eram 4 s
  para tudo). Uma conexão reaproveitada que o servidor fechou (deploy) é
  refeita na hora, sem contar como falha.
- **Reentrega sem repetir** (`command_dedupe.*`). O servidor reentrega um
  comando a cada POST até o `ack`, por até 15 min. O firmware lembra os 16
  últimos `id`s: um que ainda está na fila é ignorado; um já executado é
  confirmado de novo, sem executar — uma alimentação nunca sai duas vezes por
  causa de uma resposta perdida. Num reinício essa memória se perde, e quem
  cuida é o servidor (`cmd.interrupted`, ver `docs/arquitetura-observabilidade.md`).
- **Diagnóstico que não se perde.** Eventos de Wi-Fi sobrevivem a um POST que
  falhou (antes, `info`/`warn` iam embora junto, e o primeiro POST depois de
  reassociar costuma falhar: em 69 h com 10 quedas, nenhum `wifi.disconnected`
  chegou ao banco). `wifi.disconnected` traz o motivo do ESP-IDF e o RSSI de
  antes; `wifi.connected`, quanto tempo ficou sem Wi-Fi, o AP e o canal; e a
  volta de um período sem contato de mais de 1 min manda `api.outage_summary`
  (`outage_tracker.*`): duração, falhas por tipo, tempo sem Wi-Fi, quedas,
  motivo e faixa de RSSI.
- **Telemetria e `/status`** levam `bssid`, `channel` e
  `last_disconnect_reason`. O `/status` local também mostra o AP fixado e um
  bloco `diag` com a leitura crua do potenciômetro (`pot_raw_adc`) — girar o
  botão e ver o número mudar é o teste de bancada do pot, sem depender do
  servidor.

### Vigia de rede, e o que ainda não se explica (2.1.2)

Oito dias de produção depois da 2.1.1 (banco de 2026-10-14 a 22) mostraram que
"o controlador cai" não é reinício: houve **um** reset em oito dias (power-on,
18/10), nenhum brownout, watchdog ou panic. São 93 períodos sem contato, e em
quase todos o Wi-Fi ficou **associado** o tempo todo — `wifi_reconnects` só
subiu duas vezes no boot inteiro — com o POST expirando (`-11`, o teto de 8 s, é
o valor mais repetido: p95 de 8.014 ms em sete dos nove dias; só numa madrugada,
46% dos POSTs acima de 3 s). Mais um apagão de **21 h** em 19→20/10: o Wi-Fi
tentou se associar 10.306 vezes (motivos 2, 9 e 15, de autenticação), religou o
rádio de 10 em 10 minutos, abriu e fechou o AP de recuperação, e nada resolveu.
A 2.1.2 não promete curar o rádio; ela encurta o estrago e deixa o próximo
episódio explicável:

- **Reassociar** (`net_watchdog.h`, `wifi_reassociate`). Com o Wi-Fi de pé e os
  POSTs falhando por 2 min, desassocia e associa de novo — limpa a tabela ARP,
  o lease e os sockets dos dois lados — e não repete em menos de 5 min
  (`wifi.api_stall_reassoc`). A desassociação pedida não conta como queda.
- **Reiniciar como último recurso.** Sem nenhum POST aceito por 2 h, e **só com
  a luminária apagada**: reiniciar liga e desliga o relé, e o controlador da
  luminária troca de padrão (branca/azul/verde/vermelha) a cada liga-e-desliga
  rápido. O prazo dobra a cada reinício seguido sem recuperação (2, 4, 8, 16 e 24
  h) e a sequência, guardada na NVS, volta a zero com 30 min de contato firme. A
  volta registra `system.net_watchdog_reboot`. Com a luz acesa o vigia espera
  ela apagar; o apagão de 21 h (começou às 15:33) teria acabado por volta das
  17:30.
- **Resumo da queda com o que faltava.** `api.outage_summary` agora traz o
  código de erro das falhas que não são timeout nem conexão (`-8` é falta de
  RAM) e o **mínimo de memória livre** do período.
- **`system.heap_low_water`.** O `min_free_heap` de 756 bytes visto em produção
  não dizia quando nem por quê. Um evento por boot quando o mínimo cai de 16 KB,
  com o estado do momento (AP de recuperação, Wi-Fi, fila de eventos).
- **Autodiagnóstico do potenciômetro.** Os extremos (0 e 4095) são posições do
  botão, não defeito, e o servidor já os tratava assim: o diagnóstico marcava
  "degradado" com o botão no mínimo. Agora diz "no mínimo, onde a ventoinha
  desliga pelo botão; se ele não está no mínimo, confira o fio do cursor".

**O que os dados não explicam, e não vamos fingir que sim:** a luminária. As 33
trocas de oito dias batem com o horário e com o botão; só `light_set` escreve
no GPIO23, e cada mudança vira evento. Um liga-e-desliga rápido que o firmware
não vê só pode estar fora dele: queda de energia na tomada, ou o SSR (a corrente
de fuga dele com a carga desligada pode manter o controlador da luminária
meio acordado e contar um clique). A 2.1.2 só garante que **o firmware não
reinicia com a luz acesa**.

### O limite de refeições do alimentador (2.1.0)

O módulo do alimentador conta as refeições de 24 h — agenda, botão e app — e
recusa a quarta. O lado de cá do enlace acompanha o protocolo dele
(`feeder_link.h` tem a referência; o firmware do módulo, em
`firmware/feeder-module/`, o [README de lá](../firmware/feeder-module/README.md)):

- **`FEED [grãos] [FORCE]`.** O `feeder.feed_now` com `force: true` — o
  "alimentar mesmo assim" do app — vai com `FORCE`, e registra
  `feeder.limit_overridden`. Sem `FORCE`, o módulo decide.
- **`PONG` com as refeições em 24 h** no fim; vão no bloco `feeder` do POST
  como `meals_24h`. Um módulo antigo, sem o campo, continua aceito.
- **`FED` com motivo e origem.** `VAZIO` vira `feeder.hopper_empty` (erro),
  `SENSOR` vira `feeder.sensor_fault` — a refeição saiu, contada pelo servo —,
  e a mensagem diz de onde veio o pedido (agenda, recuperação, botão, app).
- **`DENIED`**, a recusa: `LIMITE` vira `feeder.limit_reached`, `PERDIDA`
  (refeição da agenda que não deu para recuperar) vira `feeder.meal_missed`, e
  `OCUPADO`, `CALIBRAR` ou um motivo que este firmware ainda não conhece viram
  `feeder.feed_denied`, com o motivo na mensagem. Nada de recusa silenciosa.

Os campos novos são opcionais dos dois lados: firmware 2.1.0 com módulo antigo,
ou o contrário, só perdem a informação nova.

### A ventoinha desligada de verdade, e o potenciômetro que responde (2.0.3)

**Desligar solta a linha de PWM.** O corte de energia é no retorno (pino 1,
`GPIO26`), e o PWM passa por um MOSFET que inverte o sinal. Até a 2.0.2,
desligar escrevia o complemento de 0% — o MOSFET do PWM conduzindo o tempo
todo, o pino 4 preso no `GND` —, e a ventoinha, sem o próprio terra, voltava
por ali: 3 a 7 V nos terminais e giros aos trancos, cadenciados, com o
`GPIO26` corretamente em 0 V. Agora, com a energia cortada, o duty é 0 e a
linha fica solta; ao religar, o duty é escrito antes da energia.

**O potenciômetro assume quando é girado.** Antes:

- só valia depois de ir ao mínimo e subir — de novo a cada boot (OTA
  incluída), comando do app e virada da agenda; girar de qualquer outro ponto
  não fazia nada;
- uma leitura a cada 500 ms com um filtro que levava ~6 s até o valor novo: a
  velocidade "escorregava" por segundos, e desligar no mínimo demorava o mesmo.

Agora ele é lido a cada volta do loop (200 ms), na média de 8 conversões.
Fora do comando, assume quando anda mais de 150 pontos em duas amostras
seguidas — um giro de mão; um pico de ruído ou a deriva lenta do ADC não. No
comando, a velocidade acompanha o giro na mesma volta, o mínimo desliga, e o
evento `pot.fan_speed` sai quando a mão para (eram 184 num dia de bancada). Um
comando do app, ou a volta ao automático da agenda, tira o comando dele de
novo: vale o último.

### O termômetro: leitura que passou no CRC ainda pode ser lixo (2.0.2)

O CRC-8 do DS18B20 deixa passar, por acaso, 1 em cada 256 quadros
corrompidos. Em produção apareceram dois -48,00 °C (registrador `0xFD00`)
entre leituras de 27,13 °C, cinco minutos depois de um boot, sem nenhum
`temp.*` em volta: o firmware os aceitou como leitura válida, e eles entraram
no gráfico.

- **Faixa possível:** fora de 10–45 °C (a água desta instalação não sai disso;
  `TEMP_PLAUSIBLE_MIN_C`/`MAX_C` no `config.h` para outra), a leitura é
  descartada e conta como falha — três seguidas, sensor perdido. O evento
  `temp.implausible` leva os 9 bytes crus do scratchpad, para separar CRC fraco
  de ruído no fio.
- **Salto:** uma leitura a mais de 2 °C da anterior não é aceita de cara. O
  valor anterior fica, a conversão seguinte sai na hora (sem esperar os 5 s), e
  o salto só vale se ela confirmar, a 0,5 °C. Não confirmou: `temp.spike_discarded`.
  É o que pega o quadro corrompido que caia dentro da faixa, um 20 °C por
  exemplo. Uma mudança real atrasa ~1 s.
- **Leitura pelo endereço:** o `getTempCByIndex()` refazia a busca no 1-Wire a
  cada conversão; agora o endereço é lido uma vez, na sondagem. Menos tráfego
  no fio, menos chance de corromper.

O servidor confere a mesma faixa de novo (ver `api-servidor.md`).

### O alimentador: só com ele no fio (2.0.1)

O firmware só fala do módulo do alimentador quando ele está lá, nos dois
sentidos:

- **Pelo fio.** O RX (`GPIO16`) tem pull-down. Com o conector vazio, o fio fica
  em nível baixo; com o módulo ligado, em alto, porque a UART dele repousa em
  alto. Duas amostras em alto dão o módulo como presente, e cinco em baixo
  (~1 s) como ausente: os bits de um byte duram ~104 µs e nunca enchem cinco
  amostras de ciclos diferentes. Sem módulo, nenhum `PING` sai. Com o cabo
  arrancado, a desconexão é na hora, sem esperar os 6 s de silêncio.
- **Para o servidor.** O bloco `feeder` só vai no POST com o módulo
  conectado. A agenda só vai depois do primeiro `PONG` ou `SCHEDULE` válido, e
  a última alimentação só se houve uma.

Até a 2.0.0, o `PING` saía a cada 2 s para ninguém, e o bloco ia inteiro, com
zeros. "0 grãos", abaixo do mínimo do contrato, virou um evento de correção
por POST em produção (v1.1.0), e a agenda 00h/00h apareceu no app para um
módulo que nunca existiu. O pull-up que havia no RX segurava o fio vazio
quieto, mas no mesmo nível de um módulo ocioso. O pull-down também segura o
fio quieto e ainda diferencia os dois casos.

Na bancada, `/status` mostra `feeder.present`: falso com o conector vazio.

---

## Verificado

- **Compila** contra ESP32 core 3.3.8 e ArduinoJson 7.4.3, sem nenhum aviso nos
  arquivos do projeto mesmo com `--warnings all`: 1.160.846 bytes (88% do
  flash) na 2.1.2, 58 KB de RAM global (18%).
- **Os quatro formatos de corpo** que o firmware emite (normal, sensor ausente,
  RTC sumido, com `ack` de recusa e eventos) validam contra
  `telemetryRequestSchema`. Entre 391 e 777 bytes.
- **Ponta a ponta** contra o servidor real: o corpo do firmware foi aceito, e o
  servidor chegou de forma independente ao mesmo diagnóstico que o firmware —
  ventoinha em `fault`, RTC `degraded` por bateria, Wi-Fi `degraded` por RSSI.
- **Em bancada, num ESP32 físico** (2026-10-01, com o firmware de debug, que é
  este mais a instrumentação): os dois núcleos sob carga real por horas; OTA
  repetido; tacômetro, potenciômetro e corte de energia da ventoinha; 32 min
  com 62.909 picos de ruído no botão e só as trocas dos toques reais.
- **Falhas injetadas ao vivo**: 85 °C isolado descartado e, repetido, sensor
  perdido → modo de segurança da ventoinha → recuperação sozinha; leitura
  inválida e falha de I²C do DS3231 → módulo ausente → hora seguindo pelo NTP no
  fuso certo → módulo reencontrado; hora errada isolada sem trocar a luz;
  autodiagnóstico em 163 ms com a pilha do loop no máximo a ~3,6 KB dos 8 KB;
  loop travado de propósito → reinício pelo watchdog em ~60 s.
- **Presença do alimentador** (2026-10-02, firmware 2.0.1 de produção): com o
  conector vazio, o pull-down interno basta, e `/status` mostra
  `feeder.present: false`. Não há resistor externo no `GPIO16` da placa
  montada.
- **Testes de host** (g++ no PC, com o `.cpp` de produção e stubs mínimos de
  Wire/RTClib/Serial/GPIO/ADC/LEDC/DallasTemperature): 37 verificações da
  automação por horário e da leitura do DS3231; 49 do enlace com o
  alimentador — parser, presença no fio, nenhum `PING` sem módulo, desconexão
  na hora com o cabo arrancado, o 0x00 do *break*, e o limite de refeições
  (`FEED` com `FORCE`, `PONG` com as refeições, cada `DENIED` com o seu
  evento); 21 do termômetro — o
  -48,00 de produção descartado com o scratchpad, salto relido e descartado,
  mudança real confirmada, 85,0 de power-on e sensor perdido por leituras
  impossíveis; e 20 da ventoinha — desligar soltando a linha de PWM, o pot
  assumindo em 400 ms sem passar pelo mínimo, acompanhando o giro com um
  evento só, desligando no mínimo, e ruído isolado e deriva lenta do ADC que
  não assumem.
- **Firmware 2.0.2 e 2.0.3 no ESP32 da bancada** (2026-10-02): temperatura
  válida e renovada a cada ~5 s, POST com 200; ventoinha desligada com o
  tacômetro em 0 rpm por um minuto inteiro.

## Não verificado — exige hardware

- a janela de 5 s até acusar `fan.tach_stalled` com a ventoinha travada de
  verdade;
- o cenário "servidor fora do ar por dois dias", que é o que valida o NVS como
  cache offline de verdade;
- a série de heap ao longo de dias: a planilha de medições (Registros) traz a
  memória livre; o maior bloco alocável fica em `telemetry.max_alloc_heap`.
  Maior bloco caindo junto com o livre é fragmentação;
- o enlace com o módulo do alimentador ao vivo: o firmware do módulo existe
  (1.0.0-beta, 2026-10-02) e roda na placa dele, mas o mecanismo ainda não foi
  montado, e os dois lados do enlace só se encontraram em teste de host.

**Testar em bancada antes de gravar no que está no aquário.** Para investigar
um comportamento estranho, há o firmware de debug em `firmware/bettacare-debug`
(ver o `README.md` de lá), gravável por OTA.

## Antes de gravar

1. `cp config.example.h config.h` e preencher: `WIFI_SSID`, `WIFI_PASSWORD`,
   `SERVER_HOST` (IP do homelab), `SERVER_API_TOKEN` (idêntico ao
   `DEVICE_INGEST_TOKEN` do servidor), `API_AUTH_TOKEN` e `OTA_PASSWORD`.
2. **Rotacionar a senha de OTA.** A do repositório antigo esteve versionada em
   texto claro e deve ser considerada comprometida.
3. O flash está em 87%. Há folga para OTA (as duas partições de app têm o mesmo
   tamanho), mas pouca para crescer — vale ficar de olho.
