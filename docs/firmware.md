# Firmware do ESP32

Referência da Fase 3. Complementa `arquitetura-observabilidade.md` e
`api-servidor.md`.

---

## Os dois núcleos

A decisão estrutural desta reescrita, e ela corrige um defeito real:

```
núcleo 1 — loop()          núcleo 0 — net_task
├─ botão físico            ├─ Wi-Fi (associação, reconexão, AP de recuperação)
├─ DS18B20                 ├─ POST /api/v1/telemetry
├─ ventoinha (histerese)   ├─ NTP
├─ automação por horário   └─ OTA / portal de recuperação
└─ executa comandos
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
| fila de `ack` | controle → rede | `xQueue` (8 posições) |

E mais dois recursos compartilhados, cada um com o próprio cuidado:

- **`event_log`** — buffer de 24 eventos com mutex. O controle escreve, a rede
  drena.
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
`rtc_check_automation()` (núcleo 1) e `rtc_sync_ntp()` (núcleo 0), ambos sob
`_i2c_mutex`, e nenhum dos dois registra evento enquanto o segura.

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

Sobre a re-sondagem: antes, um DS18B20 que caísse depois do boot deixava
`available` verdadeiro para sempre e as leituras apenas paravam de mudar. O
requisito de "saber quando um componente deixou de ser reconhecido" não tinha
como ser atendido sem isso.

### O `seq` e o reenvio

O `seq` **só avança quando a troca é bem-sucedida**. Se a resposta se perder, a
tentativa seguinte repete o mesmo `seq`, o servidor reconhece o reenvio pela
guarda `device_state.last_seq` e devolve a mesma lista de comandos.

### Recuo progressivo

Com o servidor fora do ar, o intervalo cresce até 30 s. Sem isso seriam 20
requisições por minuto sem propósito, cada uma com timeout de 4 s. O aquário
segue operando sozinho e volta a falar quando houver com quem.

---

## Verificado

- **Compila** contra ESP32 core 3.3.8 e ArduinoJson 7.4.3:
  1.125.830 bytes (85% do flash), 55 KB de RAM global (16%).
- **Os quatro formatos de corpo** que o firmware emite (normal, sensor ausente,
  RTC sumido, com `ack` de recusa e eventos) validam contra
  `telemetryRequestSchema`. Entre 391 e 777 bytes.
- **Ponta a ponta** contra o servidor real: o corpo do firmware foi aceito, e o
  servidor chegou de forma independente ao mesmo diagnóstico que o firmware —
  ventoinha em `fault`, RTC `degraded` por bateria, Wi-Fi `degraded` por RSSI.

## Não verificado — exige hardware

Nada disto foi exercitado num ESP32 físico:

- comportamento dos dois núcleos sob carga real (o compilador não prova ausência
  de deadlock, só de erro de tipo);
- leitura do tacômetro e a janela de 5 s até acusar `fan.tach_stalled`;
- calibração do potenciômetro;
- OTA a partir do firmware novo;
- o cenário "servidor fora do ar por dois dias", que é o que valida o NVS como
  cache offline de verdade.

**Testar em bancada, com o ESP32 no USB, antes de gravar no que está no
aquário.** Se o firmware novo quebrar a rede, o OTA não salva.

## Antes de gravar

1. `cp config.example.h config.h` e preencher: `WIFI_SSID`, `WIFI_PASSWORD`,
   `SERVER_HOST` (IP do homelab), `SERVER_API_TOKEN` (idêntico ao
   `DEVICE_INGEST_TOKEN` do servidor), `API_AUTH_TOKEN` e `OTA_PASSWORD`.
2. **Rotacionar a senha de OTA.** A do repositório antigo esteve versionada em
   texto claro e deve ser considerada comprometida.
3. O flash está em 85%. Há folga para OTA (as duas partições de app têm o mesmo
   tamanho), mas pouca para crescer — vale ficar de olho.
