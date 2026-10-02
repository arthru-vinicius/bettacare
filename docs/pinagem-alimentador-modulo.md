# Pinagem e Montagem — Módulo Alimentador (ESP32-C3 Mini)

Guia de montagem física do módulo opcional de alimentação de precisão do
BettaCare. Companheiro de
[`pinagem-e-montagem-esp32.md`](./pinagem-e-montagem-esp32.md) — aqui é só o
módulo que fica na caixinha separada, ligado ao principal por 4 fios.

> **Criado em 2026-09-03, revisado em 2026-09-03.** Módulo pensado para
> funcionar de forma totalmente independente do ESP32 principal — RTC, botão
> e display próprios. A interligação é opcional: sem o cabo conectado, o
> módulo continua alimentando o betta sozinho.
>
> **Controlador trocado de Arduino Pro Mini para ESP32-C3 Mini** nesta
> revisão — Pro Mini de verdade saiu caro ou sem USB embutido nas buscas do
> usuário. O ESP32-C3 resolve isso (USB-C embutido, ~R$30, entrega nacional)
> e ainda reaproveita o mesmo toolchain e o mesmo mecanismo de memória
> (NVS/`Preferences`) que o ESP32 principal já usa. WiFi/Bluetooth do chip
> ficam sem uso — sem custo real em troca disso.
>
> **O módulo não fica ligado o tempo todo.** Liga quando for prático — isso
> exige duas garantias no firmware que não existiriam se ele ficasse sempre
> energizado: M1 precisa disparar pelo menos 1×/dia mesmo sem alimentação
> completa (anti-empacamento não pode depender do resto do ciclo), e ao
> ligar o módulo deve recuperar uma refeição perdida assim que possível, não
> esperar o próximo horário fixo. Comportamento de firmware — pendência
> registrada em memória do projeto, este documento é só a pinagem.

---

## Pinagem (ESP32-C3 Mini)

Evitados de propósito: `GPIO2`, `GPIO8`, `GPIO9` (strapping — afetam o modo
de boot), `GPIO12`–`GPIO17` (reservados à flash SPI embutida na maioria das
placas), `GPIO18`/`GPIO19` (D+/D- do USB nativo, se a placa usar USB nativo
em vez de ponte CH340/CP2102).

| Pino (GPIO) | Função | Observação |
|---|---|---|
| 4 | Serial — TX pro ESP32 principal | UART1, separado do UART usado pra gravação/console |
| 5 | Serial — RX do ESP32 principal | idem |
| 6 | SDA — DS3231 + SSD1306 | I²C, barramento compartilhado |
| 7 | SCL — DS3231 + SSD1306 | idem |
| 10 | Servo MG90S (PWM/LEDC) | direto — servo tem alimentação própria |
| 0 | M1 — vibração anti-empacamento | via transistor NPN, nunca direto no GPIO |
| 1 | M2 — vibração de aviso (pêndulo) | via transistor NPN, nunca direto no GPIO |
| 3 | Sensor IR (saída do LM393) | interrupção — conta grão a grão |
| 11 | Push button | `INPUT_PULLUP`, amostrado por timer como o botão da luz no ESP32 principal — **⚠️ o `GPIO11` é o `VDD_SPI` no C3, ver §6** |
| 5V / VIN | Alimentação — **5V já regulado, não 12V bruto** | ver §1 |
| 3V3 | Saída do regulador onboard → sensor IR + LM393 | mesmo nível lógico das entradas digitais, sem divisor de tensão |

> **Confira contra o pinout serigrafado da placa específica antes de soldar.**
> Placas "ESP32-C3 Mini"/"SuperMini" variam de fabricante pra fabricante —
> nem todos os GPIOs do chip são necessariamente expostos, e a numeração
> impressa na placa às vezes não bate 1:1 com o número do GPIO. Os números
> acima são do chip, corretos independente da placa; o mapeamento físico
> pino-a-pino é o que precisa checar na hora.

---

## Lista de componentes

| Componente | Qtd. | Situação |
|---|---|---|
| ESP32-C3 Mini (USB-C) | 1 | comprar |
| Servo MG90S (engrenagem metálica) | 1 | comprar |
| Motor de vibração 1027 (moeda) — M1 | 1 | comprar |
| Motor de vibração DC130P (pêndulo) — M2 | 1 | comprar |
| Módulo RTC DS3231 | 1 | **já possui** |
| Push button N.O. | 1 | **já possui** |
| Display OLED SSD1306 0,96" 128×64 I²C | 1 | **já possui** |
| Par LED IR emissor + receptor 5mm | 1 | comprar |
| Módulo comparador LM393 | 1 | comprar |
| Módulo step-down LM2596 | 1 | comprar |
| Transistor NPN BC337 | 2 | comprar |
| Diodo 1N4148 | 2 | comprar |
| Resistor 220 Ω | 1 | comprar |
| Resistor 1 kΩ | 2 | comprar |
| Resistor 10 kΩ | 1 | comprar |
| Capacitor eletrolítico 100–220 µF / 16V | 1 | comprar |
| Capacitor cerâmico 100 nF (104) | 1 | comprar |
| Conector JST-XH 4 vias (macho + fêmea) | 1 par | comprar |
| Bateria CR2032 (do DS3231, se ainda não tiver) | 1 | conferir |

> Modelos com custo-benefício comparado e links de compra ficaram registrados
> na conversa em que este módulo foi desenhado — esta tabela é o inventário
> final pra bancada, não repete a pesquisa de preço.

---

## 1. Alimentação

O módulo recebe **12V bruto** pelo conector de 4 vias — a regulação acontece
inteira aqui dentro, não no módulo principal.

```text
12V (do conector) ──► LM2596 ──► 5V ──┬──► 5V/VIN do ESP32-C3
                                      ├──► V+ do servo
                                      └──► coletor de M1 / M2 (via transistor)

3V3 (do ESP32-C3) ──┬──► LED IR (com resistor 220Ω em série)
                     └──► VCC do LM393 / fototransistor
```

- **Não** ligue os 12V brutos direto no `5V`/`VIN` do ESP32-C3 — regule pra
  5V primeiro, no LM2596. O regulador onboard da placa é pra cair 5V→3,3V,
  não 12V→3,3V de uma vez.
- Capacitor de 100–220 µF entre `V+` do servo e `GND`, o mais perto possível
  do servo — absorve o pico de corrente do movimento sem afundar o rail que
  também alimenta o ESP32-C3.
- **Terra em estrela também aqui**: o retorno de M1/M2 (motor de escova, mais
  ruidoso eletricamente que a ventoinha BLDC do módulo principal) não
  compartilha fio físico com o retorno do sensor IR nem com o do I²C.

---

## 2. Servo (doseador) — GPIO10

Sinal direto, sem transistor — servo já espera nível lógico e puxa a própria
corrente do `V+`, não do pino de sinal.

```text
GPIO10 ───────────────► sinal (fio laranja/amarelo)
5V (LM2596) ─────────► V+ (fio vermelho)
GND ─────────────────► GND (fio marrom/preto)
```

Ângulos de repouso e despejo são calibrados na bancada, não fixos em
firmware — ver a pendência de calibração local em memória do projeto.

---

## 3. M1 e M2 (motores de vibração) — GPIO0 / GPIO1

Motor DC não liga direto num GPIO — a corrente passa pelo transistor, nunca
pelo pino.

```text
GPIO0 (ou GPIO1) ──── 1k ──── base
                                │        [BC337]
5V ──── M1/M2 ──── coletor
                                │
                             emissor
                                │
                               GND

Diodo 1N4148 em paralelo com o motor, catodo para o lado de +5V
(roda-livre contra a indução no desligar)
```

- **M1** fica colado na parede do funil do reservatório — nunca toca água.
  Dispara pelo menos 1×/dia mesmo sem alimentação completa (o módulo não
  fica sempre ligado — ver nota no topo do documento).
- **M2** fica na ponta do tubo de queda — é o único ponto do módulo inteiro
  que encosta na água.

---

## 4. Sensor de contagem (IR + LM393) — GPIO3

```text
3V3 ──── 220Ω ──── LED IR (ânodo)         fototransistor (coletor)
                       │                            │
                      GND (catodo)                10k pull-up ── 3V3
                                                     │
                                          entrada do LM393 (pino 2)
                                                     │
                                          saída do LM393 (pino 1) ──► GPIO3
```

- LED e fototransistor ficam em lados opostos do tubo, alinhados — feixe
  atravessando, não refletindo.
- Trimpot do módulo LM393 ajusta o limiar — gire até a saída virar de nível
  limpo (sem oscilar) com o feixe livre, e o oposto com o feixe bloqueado por
  um grão de teste.
- Mantenha esse trecho do tubo fechado à luz externa — luz ambiente entrando
  pela abertura do aquário pode mascarar o sinal.

---

## 5. RTC (DS3231) e display (SSD1306) — GPIO6/GPIO7

Mesmo barramento I²C, endereços diferentes (`0x68` o RTC, `0x3C` o display,
normalmente) — sem conflito.

```text
3V3 ──┬──── VCC (DS3231)
      └──── VCC (SSD1306)

GPIO6 ┬──── SDA (DS3231)
      └──── SDA (SSD1306)

GPIO7 ┬──── SCL (DS3231)
      └──── SCL (SSD1306)

GND ──┴──── GND (os dois)
```

> Mesma ressalva do RTC do módulo principal: se este DS3231 for do tipo
> ZS-042 com o circuito de recarga pra bateria LIR2032, mantenha em 3,3V com
> CR2032. Ver a nota completa em `pinagem-e-montagem-esp32.md`, §3.

**Tela apagada por padrão.** O OLED só acende ao interagir com o botão, fica
5s aceso e apaga de novo (exceto em modo de programação, que usa uma janela
de inatividade de 20s própria) — comportamento de firmware, ver memória do
projeto. Não precisa de nenhum componente adicional pra isso.

---

## 6. Botão físico — GPIO11

```text
GPIO11 ──┬──── botão ──── GND
         │
       100nF
         │
        GND
```

Mesmo padrão do botão da luz no módulo principal **na versão atual**:
`INPUT_PULLUP` com o nível **amostrado por timer** (2 ms; toque = 30 ms
seguidos em baixo, soltura = 30 ms seguidos em alto) — não interrupção na
borda com debounce de 50 ms, que era a recomendação anterior deste documento.
No módulo principal, a interrupção aceitou como toque os picos que o 1-Wire
vizinho induzia no fio e fazia o relé trocar sozinho (UPGRADE/07; ver
`light.cpp` e `pinagem-e-montagem-esp32.md`, §2). Aqui o servo e os motores
M1/M2 são fontes de ruído ainda piores. O **significado** do toque (1×/2×/3×,
avançar/confirmar) é comportamento de firmware, a definir junto com ele.

> ⚠️ **Confira o `GPIO11` antes de soldar.** No ESP32-C3 ele é o pino
> `VDD_SPI`, que alimenta a flash; só vira GPIO comum queimando um eFuse, e
> nas variantes com flash dentro do chip — as das placas Super Mini — nem
> isso, porque é ele que alimenta essa flash. A Super Mini também não costuma
> expor o `GPIO11`. Escolha outro pino livre da
> sua placa para o botão — o `GPIO20` ou o `GPIO21`, se a serial de gravação
> for a USB nativa (como nesta placa, com `CDCOnBoot`) — e atualize a tabela
> de pinagem acima.

---

## 7. Conector com o módulo principal (4 vias)

Na ordem física do conector montado (a mesma tabela de
`pinagem-e-montagem-esp32.md`, §8):

| Pino | Cor | Sinal | Módulo principal (ESP32) | Este módulo (ESP32-C3) |
|---|---|---|---|---|
| 1 | preto | `GND` | `GND` comum | `GND` |
| 2 | verde | `12V` | `+12V` bruto da fonte | entrada do regulador de 5 V (§1) |
| 3 | vermelho | `TX` → | `GPIO4` (`TXD2`) | `GPIO5` (RX) |
| 4 | azul | ← `RX` | `GPIO16` (`RXD2`) | `GPIO4` (TX) |

- **Conector com chave** (não use Dupont solto) — inverter 12V com GND aqui
  queima os dois módulos.
- Fios 1 e 2 vêm direto da fonte de 12V do módulo principal, **antes** do
  buck que alimenta o ESP32 principal — não depois. Os 12 V viajam brutos de
  propósito: com corrente menor no cabo, a queda é menor, e o regulador local
  entrega 5 V limpos ali mesmo, longe do ruído dos motores M1/M2. Se o
  problema for espaço na caixinha, troque o LM2596 por um buck fixo de 5 V
  menor — não mande 5 V pelo cabo.
- Protocolo, uma linha ASCII por mensagem a 9600 baud: o principal manda
  `PING`, `FEED [grãos]` e `CFG <h1> <h2> <grãos> <0|1>`; este módulo responde
  `PONG <h1> <h2> <grãos> <auto> <idade_s> <req> <conf> <ok>`, `SCHEDULE <h1>
  <h2> <grãos> <auto>` (a cada reconexão) e `FED <req> <conf> <ok>`. O lado do
  principal já está implementado e descarta linha com campo fora da faixa —
  referência completa no cabeçalho de `firmware/bettacare/feeder_link.h`.
- A atribuição de `GPIO4`/`GPIO16` no lado do ESP32 principal está registrada
  em `pinagem-e-montagem-esp32.md`, seção de interligação — são pinos do
  *outro* chip, não confundir com o `GPIO4` deste documento (chips
  diferentes, numeração independente).

---

## Roteiro de bancada

Teste o módulo **isolado**, sem o conector de 4 vias plugado no módulo
principal, antes de integrar.

| # | Medição | Instrumento | O que decide |
|---|---|---|---|
| 1 | Tensão de saída do LM2596, antes de plugar o ESP32-C3 | Multímetro | Precisa estar em **5,0V ± 0,1V** — ajuste o trimpot antes de conectar qualquer coisa a jusante |
| 2 | Continuidade do conector JST-XH pino a pino | Multímetro (modo continuidade) | Confirma que não há inversão antes da primeira conexão com o módulo principal |
| 3 | Ciclo do servo em bancada, sem grão | Visual | Curso de repouso → despejo completo, sem travar |
| 4 | Saída do LM393 com o feixe livre / bloqueado | Multímetro ou LED de teste | Nível limpo nos dois estados, sem oscilar |
| 5 | Pulso de M1 e de M2 ao energizar | Visual/tátil | Vibração perceptível nos dois, sem forçar corrente além do datasheet |
| 6 | Barramento I²C — scan de endereços | Monitor serial (USB-C) | Deve enxergar `0x68` (DS3231) e `0x3C` (SSD1306) |
| 7 | Console/gravação via USB-C simultâneo ao link serial com o módulo principal | Monitor serial + osciloscópio (ou só observação) | Confirma que gravar uma versão nova não exige desconectar o conector de 4 vias |

**Faça a 1 e a 2 antes de qualquer outra coisa.** São as que evitam queimar um
componente por engano de fiação.

---

## Depois de montar

1. Grave o firmware do ESP32-C3 pelo USB-C.
2. Calibre o servo em modo local (botão + monitor serial).
3. Confirme o roteiro de bancada acima com o módulo isolado.
4. Só depois conecte o cabo de 4 vias no módulo principal e confirme a
   handshake (`PING`/resposta) antes de fechar a caixinha.
