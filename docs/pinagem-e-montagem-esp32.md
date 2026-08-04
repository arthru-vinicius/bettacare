# Pinagem e Montagem — ESP32-WROOM-32D

Documento de referência da montagem física do sistema na versão atual:

- `DS18B20` no `GPIO19`
- ventoinha `12V 4 pinos (CPU fan)` com `PWM direto` no `GPIO17` (pino 4 da fan)
- tacômetro no `GPIO25` (pino 3 da fan) — RPM medido e exibido na interface

---

## Aviso de segurança (AC)

O `SSR40DA` comuta tensão de rede (`127V/220V AC`).

- Não tocar no lado AC energizado.
- Usar isolamento físico entre baixa tensão (ESP32/sensores) e rede AC.
- Interromper apenas a fase da luminária no SSR.

---

## Pinagem final (firmware atual)

| GPIO | Função | Observação |
|---|---|---|
| 18 | Push button | `INPUT_PULLUP`, botão para GND |
| 21 | SDA (DS3231) | I2C |
| 22 | SCL (DS3231) | I2C |
| 23 | SSR40DA entrada + | Em série com resistor `220R` |
| 19 | DS18B20 DATA | OneWire |
| 34 | Potenciômetro (wiper) | ADC1, seguro com Wi-Fi |
| 17 | PWM control (pino 4 da fan) | PWM LEDC 25 kHz, direto sem MOSFET |
| 25 | Tach (pino 3 da fan) | `INPUT_PULLUP`, sinal open-drain da fan |

---

## Componentes do sistema

### Sistema 1 — Luminária

- ESP32 DevKit (WROOM-32D)
- DS3231SN
- CR2032
- SSR40DA
- Push button N.O.
- resistor `220R` (GPIO23 -> SSR+)
- resistor `10k` pulldown (GPIO23 -> GND)
- capacitores `100nF` para desacoplamento

### Sistema 2 — Temperatura + Fan

- DS18B20 (3 fios)
- potenciômetro B10K
- ventoinha 12V 4 pinos (CPU fan — conector padrão)
- fonte 12V para fan (GND comum com ESP32)
- capacitor `100nF` no ADC do potenciômetro (filtro)
- capacitor `100nF` próximo ao DS18B20 (desacoplamento)
- capacitor `100nF` + `10uF` no conector da fan (recomendado)

---

## Ligações

### 1) Alimentação e desacoplamento

Base:

- `3V3` do ESP32 alimenta DS3231, DS18B20 e potenciômetro
- `GND` comum para todos os módulos de baixa tensão

Desacoplamento com `100nF`:

- um capacitor entre `3V3` e `GND` próximo ao ESP32
- um capacitor entre `3V3` e `GND` próximo ao DS3231

Observação:

- capacitor de `100nF` cerâmico (ex.: `104`) não tem polaridade

### 2) Push button (GPIO18)

Ligações:

- `GPIO18` -> terminal A do botão
- terminal B do botão -> `GND`
- capacitor `100nF` entre `GPIO18` e `GND` (opcional, debounce por hardware)

Diagrama:

```text
GPIO18 ----+---- botão ---- GND
           |
         100nF
           |
          GND
```

### 3) DS3231 (RTC)

Ligações:

- `GPIO21` -> `SDA`
- `GPIO22` -> `SCL`
- `3V3` -> `VCC`
- `GND` -> `GND`

Observação:

- módulos DS3231 normalmente já têm pull-up I2C onboard

### 4) SSR40DA (luminária)

Lado de controle (baixa tensão):

- `GPIO23` -> resistor `220R` -> `SSR +`
- `GND ESP32` -> `SSR -`
- resistor `10k` entre `GPIO23` e `GND` (pulldown)

Diagrama:

```text
GPIO23 ----+---- 220R ---- SSR +
           |
          10k
           |
GND -------+-------------- SSR -
```

Lado AC:

- fase da rede -> terminal 1 do SSR
- terminal 2 do SSR -> fase da luminária
- neutro da rede -> neutro da luminária

### 5) DS18B20 (GPIO19)

Ligações:

- `GPIO19` -> `DATA`
- `3V3` -> `VCC`
- `GND` -> `GND`
- capacitor `100nF` entre `VCC` e `GND` próximo ao sensor

Se o sensor não tiver pull-up embutido:

- adicionar `4,7k` entre `DATA` e `3V3`

### 6) Potenciômetro B10K (GPIO34)

Ligações:

- lateral 1 -> `3V3`
- lateral 2 -> `GND`
- pino central (wiper) -> `GPIO34`
- capacitor `100nF` entre `GPIO34` (wiper) e `GND`

Diagrama:

```text
3V3 ----[lateral]   B10K   [lateral]---- GND
                 \         /
                  \ wiper /
                   GPIO34
                     |
                   100nF
                     |
                    GND
```

Sentido de rotação:

- para aumentar no sentido horário: deixar `3V3` no lado direito (olhando o eixo de frente)
- se inverter, troque apenas os dois pinos laterais

### 7) Ventoinha 12V 4 pinos (CPU fan)

Topologia: PWM direto no pino de controle da fan — sem MOSFET.

O `+12V` fica sempre ligado. O ESP32 envia o sinal PWM (3,3V, 25 kHz) diretamente para o
pino 4 do conector, e lê os pulsos do tacômetro no pino 3.

Ligações por pino do conector da fan:

- Pino 1 (GND — fio preto) -> `GND` da fonte 12V (GND comum com ESP32)
- Pino 2 (+12V — fio amarelo) -> `+12V` da fonte (sempre ligado)
- Pino 3 (Tach — fio verde) -> `GPIO25` (pull-up interno ativo — sem resistor externo)
- Pino 4 (PWM — fio azul) -> `GPIO17` (direto, sem resistor de gate)

Desacoplamento da linha 12V:

- recomendado: `100nF` + `10uF` entre `+12V` e `GND` próximo ao conector da fan
- o `10uF` absorve transientes maiores; o `100nF` filtra ruído de alta frequência

GND comum:

- `GND` da fonte 12V deve ser conectado ao `GND` do ESP32 (referência compartilhada)

Diagrama:

```text
                +12V fonte
                   |
                   +----[100nF]----+----[10uF]---- GND
                   |               |
                   +---------------+-- Pino 2 (+12V) fan

GND fonte --------------------------+-- Pino 1 (GND) fan
                                    |
                                    = GND comum ---- GND ESP32

GPIO17 ------------------------------------- Pino 4 (PWM control)
GPIO25 ------------------------------------- Pino 3 (Tach)
  (INPUT_PULLUP interno ~45 kΩ — pull-up externo dispensável)
```

---

## Notas de firmware (fan 4 pinos)

A API permanece:

- `GET /fan_toggle`
- `GET /fan_speed?value=0..100`

A resposta JSON inclui agora o campo `rpm`:

```json
"fan": { "on": true, "speed_percent": 60, "rpm": 1450, ... }
```

O tacômetro usa o periférico de interrupção do ESP32 para contar os pulsos
do pino 3 (open-drain, 2 pulsos por volta). O RPM é amostrado a cada 2 segundos.

Diferenças em relação à fan 2 fios:

- não há impulso de partida (a fan 4 pinos tem controlador interno)
- não há piso mínimo de duty (o controlador da fan cuida da velocidade mínima)
- duty `0%` desliga a fan; `100%` é velocidade máxima