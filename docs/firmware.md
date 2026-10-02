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

- **`fan.cpp`** — histerese, cooldown de 30 min, escalonamento progressivo,
  failsafe de temperatura indisponível e a calibração do potenciômetro
  (min → cima). É a lógica mais madura do projeto. Mudou só de onde vêm os
  limiares: agora de `device_config`, ou seja, do servidor.
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
| Qualquer leitura do DS18B20 aceita | 85,0 °C (valor de power-on) descartado: `temp.reset_value` |

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

Com o servidor fora do ar, o intervalo cresce até 30 s. Sem isso seriam 20
requisições por minuto sem propósito, cada uma com timeout de 4 s. O aquário
segue operando sozinho e volta a falar quando houver com quem.

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
  arquivos do projeto mesmo com `--warnings all`: 1.137.662 bytes (86% do
  flash), 57 KB de RAM global (17%).
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
  Wire/RTClib/Serial/GPIO): 37 verificações da automação por horário e da
  leitura do DS3231, e 37 do enlace com o alimentador: parser, presença no
  fio, nenhum `PING` sem módulo, desconexão na hora com o cabo arrancado e o
  0x00 do *break*.

## Não verificado — exige hardware

- a janela de 5 s até acusar `fan.tach_stalled` com a ventoinha travada de
  verdade;
- o cenário "servidor fora do ar por dois dias", que é o que valida o NVS como
  cache offline de verdade;
- a série de heap ao longo de dias (a aba Relatórios responde: maior bloco
  livre caindo junto com o livre é fragmentação);
- o enlace com o módulo do alimentador, cujo firmware ainda não existe — o lado
  do ESP32 principal só foi exercitado por teste de host.

**Testar em bancada antes de gravar no que está no aquário.** Para investigar
um comportamento estranho, há o firmware de debug em `firmware/bettacare-debug`
(ver o `README.md` de lá), gravável por OTA.

## Antes de gravar

1. `cp config.example.h config.h` e preencher: `WIFI_SSID`, `WIFI_PASSWORD`,
   `SERVER_HOST` (IP do homelab), `SERVER_API_TOKEN` (idêntico ao
   `DEVICE_INGEST_TOKEN` do servidor), `API_AUTH_TOKEN` e `OTA_PASSWORD`.
2. **Rotacionar a senha de OTA.** A do repositório antigo esteve versionada em
   texto claro e deve ser considerada comprometida.
3. O flash está em 86%. Há folga para OTA (as duas partições de app têm o mesmo
   tamanho), mas pouca para crescer — vale ficar de olho.
