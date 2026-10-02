# Pinagem e Montagem — Módulo Alimentador (ESP32-C3 Super Mini)

Guia de montagem física do módulo opcional de alimentação de precisão do
BettaCare. Companheiro de
[`pinagem-e-montagem-esp32.md`](./pinagem-e-montagem-esp32.md) — aqui é só o
módulo que fica na caixinha separada, ligado ao principal por 4 fios.

> **Criado em 2026-09-03, reorganizado em 2026-10-02**, antes de qualquer
> solda, com um critério só: durar. A placa já existe (Tenstar ESP32-C3 Super
> Mini) e roda o firmware completo, em beta, de `firmware/feeder-module/` —
> o [README de lá](../firmware/feeder-module/README.md) tem a gravação, o
> console e a primeira montagem na bancada. O resto ainda não foi montado. O
> módulo funciona sozinho — RTC, botão e display próprios — e o cabo com o
> principal é opcional. A estrutura impressa (base, torre e topo) está em
> [`modelagem-alimentador/`](./modelagem-alimentador/).
>
> **O módulo não fica ligado o tempo todo.** Liga quando for prático — isso
> exige duas garantias no firmware que não existiriam se ele ficasse sempre
> energizado: M1 precisa disparar pelo menos 1×/dia mesmo sem alimentação
> completa (anti-empacamento não pode depender do resto do ciclo), e ao
> ligar o módulo deve recuperar uma refeição perdida assim que possível, não
> esperar o próximo horário fixo.

---

## O que mudou na reorganização de 2026-10-02

| Antes | Agora | Por quê |
|---|---|---|
| Botão no `GPIO11` | `GPIO5` | O `GPIO11` é o `VDD_SPI`, que alimenta a flash da placa; a Super Mini nem o expõe |
| Enlace na UART1, `GPIO4`/`GPIO5` | `GPIO20`/`GPIO21` (os pinos da UART0, usados pela UART1), com resistor em série nos dois fios | O `GPIO21` solta o log do ROM a cada reset: fica com o que tolera isso, e dois pinos quietos sobram para os atuadores. A UART0 em si fica com o console do sistema, cujo log de erro corromperia uma linha do protocolo. Os resistores protegem a ligação a quente e mantêm a detecção do módulo pelo principal (§7) |
| LED IR sempre aceso, no `3V3` | Ligado pelo `GPIO4`, só durante a dosagem | Vida do LED, menos luz parasita e um autoteste do sensor antes de cada refeição |
| BC337 com 1 kΩ e 1N4148 nos dois motores | Resistor de base por motor, pull-down na base, 1N5819 e PWM | O 1N4148 é pequeno para o pico do DC130P; sem pull-down o motor pode dar um tranco no boot; o 1027 é motor de 3 V |
| Servo direto no `GPIO10` | 330 Ω em série e 10 kΩ ao GND | Sem tranco no boot; o firmware solta o PWM depois de cada movimento |
| 12 V só pelo cabo | Cabo **ou** fonte própria de 12 V, com diodos, fusível e capacitor de entrada | "Funciona sozinho sem o cabo" precisava de uma fonte. Os diodos deixam as duas ligadas sem conflito, e um curto aqui não derruba o principal |
| LM2596 ajustável | Versão fixa de 5 V, ou o trimpot travado | Trimpot sob vibração (M1, M2, servo) desregula com o tempo |
| Pull-ups de entrada no RX do principal | Pull-down (firmware 2.0.1 do principal) | O principal detecta o módulo pelo nível do fio e não fala com quem não está lá |

---

## A placa: ESP32-C3 Super Mini

Expõe `5V`, `GND`, `3V3` e os GPIOs 0 a 10, 20 e 21. Nem todos servem:

| Pino | O que é | Uso aqui |
|---|---|---|
| `GPIO2` | *strapping*: precisa estar em nível alto no reset para dar boot | nada ligado |
| `GPIO8` | *strapping*; LED azul da placa, aceso em nível baixo | LED de status, sem nada externo |
| `GPIO9` | *strapping* (com pull-up interno); botão BOOT da placa | só o BOOT |
| `GPIO11` | `VDD_SPI`: alimenta a flash; não exposto | — |
| `GPIO12`–`GPIO17` | flash SPI; não expostos | — |
| `GPIO18`/`GPIO19` | USB nativo (gravação e console); não expostos | — |
| `GPIO20`/`GPIO21` | UART0; o ROM manda o log de boot pelo `GPIO21` a cada reset | enlace com o principal, pela UART1 |
| `GPIO0`, `1`, `3`–`7`, `10` | sem função de boot | atuadores, sensores, I²C, botão |

Os oito pinos sem função de boot (0, 1, 3 a 7 e 10) e o par 20/21 estão todos em uso. Se um
periférico novo aparecer, ele entra por I²C (um expansor como o PCF8574), nunca
num pino de *strapping*: um circuito que force o nível errado no reset impede a
placa de dar boot.

> **Confira contra a serigrafia da sua placa antes de soldar.** Placas "Super
> Mini" variam de fabricante para fabricante na ordem dos pinos e no LED; os
> números acima são do chip.

---

## Pinagem

| GPIO | Função | Ligação | Por que este pino |
|---|---|---|---|
| 0 | M1 — vibração anti-empacamento | base do BC337 por 1 kΩ, 10 kΩ ao GND; PWM | sem função de boot; o 10 kΩ segura o motor parado no reset |
| 1 | M2 — vibração de aviso (pêndulo) | base do BC337 por 470 Ω, 10 kΩ ao GND; PWM com rampa | idem |
| 3 | Sensor IR — saída do LM393 | entrada com pull-up interno, interrupção | sem função de boot; é ADC1, então o fototransistor pode ser lido direto no futuro sem refazer a fiação. O pull-up faz o fio solto reprovar o autoteste em vez de flutuar |
| 4 | LED IR | 150 Ω, aceso só na dosagem e no autoteste | sem função de boot |
| 5 | Botão | `INPUT_PULLUP`; 1 kΩ em série e 100 nF ao GND | sem função de boot; substitui o `GPIO11` |
| 6 | SDA — DS3231 + SSD1306 | I²C | sem função de boot |
| 7 | SCL — DS3231 + SSD1306 | I²C | idem |
| 10 | Servo MG90S — sinal | 330 Ω em série, 10 kΩ ao GND | sem função de boot; o 10 kΩ segura o servo no reset |
| 20 | RX ← do principal | UART1, 4,7 kΩ em série | pino da UART0, usado pela UART1 |
| 21 | TX → para o principal | UART1, 1 kΩ em série | o log do ROM no boot vai ao principal, que o descarta; depois do boot, só o protocolo |
| 8 | LED da placa | — | aceso na refeição; piscando quando há algo para ver |
| 2, 9 | — | nada externo | *strapping* |
| 5V | entrada da placa | do buck por um 1N5819 (§1) | |
| 3V3 | saída do regulador da placa | DS3231, SSD1306, LM393 e fototransistor | |

Os mesmos números estão em `firmware/feeder-module/config.example.h`.

---

## Lista de componentes

| Componente | Qtd. | Situação |
|---|---|---|
| ESP32-C3 Super Mini (USB-C) | 1 | **já possui** (Tenstar, já com o firmware 1.0.0-beta) |
| Servo MG90S (engrenagem metálica) | 1 | comprar |
| Motor de vibração 1027 (moeda) — M1 | 1 | comprar |
| Motor de vibração DC130P (pêndulo) — M2 | 1 | comprar |
| Módulo RTC DS3231 (ZS-042) | 1 | **já possui** |
| Push button N.A. de painel | 1 | **já possui** |
| Display OLED SSD1306 0,96" 128×64 I²C | 1 | **já possui** |
| Par LED IR emissor + fototransistor 5 mm | 1 | comprar |
| Módulo comparador LM393 | 1 | comprar |
| Buck 12 → 5 V, ≥ 2 A (LM2596 de 5 V fixo, ou ajustável com o trimpot travado) | 1 | comprar |
| Transistor NPN BC337 | 2 | comprar |
| Diodo Schottky 1N5822 (3 A) | 2 | comprar |
| Diodo Schottky 1N5819 (1 A) | 3 | comprar |
| Fusível rearmável (PTC) 0,75 A, ≥ 16 V | 1 | comprar |
| Jack P4 fêmea de painel (5,5 × 2,1 mm), para a fonte própria | 1 | comprar |
| Resistores: 150 Ω (1), 330 Ω (1), 470 Ω (1), 1 kΩ (3), 4,7 kΩ (1), 10 kΩ (4) | 11 | comprar |
| Eletrolítico 470 µF / 25 V (entrada de 12 V) | 1 | comprar |
| Eletrolítico 470 µF / 16 V, baixa ESR (servo) | 1 | comprar |
| Cerâmico 100 nF (104) | 4 | comprar |
| Conectores JST-XH com chave: 4 vias (cabo do principal e I²C), 3 vias (servo, LM393), 2 vias (M1, M2, LED IR, botão) | 1 par de cada | comprar |
| Bateria CR2032 (do DS3231) | 1 | conferir |
| Placa perfurada, caixa, sílica-gel, verniz acrílico | — | comprar |

Os resistores de 10 kΩ: pull-down das duas bases, pull-down do servo e
pull-up do fototransistor. Os de 1 kΩ: base do M1, TX do enlace e botão.

---

## 1. Alimentação

```text
12 V do cabo (pino 2) ───►|── 1N5822 ──┐
                                       ├──► PTC 0,75 A ──┬──► buck 12→5 V
12 V do jack (opcional) ─►|── 1N5822 ──┘                 │
                                              470 µF/25 V ao GND

buck 5 V ──┬──► V+ do servo (470 µF/16 V + 100 nF junto ao conector)
           ├──► M1 e M2 (pelos BC337, §3)
           └──►|── 1N5819 ──► pino 5V da Super Mini

3V3 da Super Mini ──► DS3231, SSD1306, LM393 e fototransistor

GND do cabo (pino 1), do jack e de tudo ──► um ponto só, na saída do buck
```

- **Os dois 1N5822 fazem um "ou".** A fonte presente alimenta; com as duas,
  vence a de tensão maior, e nenhuma empurra corrente na outra: os 12 V do
  jack nunca sobem pelo cabo até o principal. De quebra, protegem contra fonte
  invertida.
- **O PTC é o que impede o módulo de derrubar o aquário.** Pelo cabo, os 12 V
  vêm direto da fonte do principal, a mesma que alimenta o buck do ESP32
  principal. Um servo travado ou um fio esmagado aqui puxaria a fonte para
  baixo e reiniciaria o controlador da luz e da ventoinha. O fusível abre
  antes, e se rearma sozinho quando o defeito sai.
- **470 µF na entrada do buck:** o cabo tem indutância, e o servo puxa picos.
- **Buck:** prefira a versão fixa de 5 V. Se for o ajustável, ajuste em
  5,0 V **antes** de ligar qualquer coisa e trave o trimpot com uma gota de
  verniz ou esmalte — M1, M2 e o servo vibram a caixa o dia inteiro.
- **O 1N5819 antes do pino 5V da placa** deixa o USB-C conviver com o buck:
  ligando o cabo USB para gravar, a placa fica com a tensão maior, e o buck
  nunca empurra corrente para dentro do computador. A queda de ~0,3 V não
  importa, porque o regulador da placa leva a 3,3 V.
- **Servo e motores saem direto do buck**, nunca do pino 5V da placa: as
  trilhas dela são finas, e cada partida de motor viraria uma queda no ESP32-C3.
- **Terra em estrela:** o retorno de M1, M2 e do servo vai por fios próprios
  até o ponto de terra, sem dividir fio com o sensor IR nem com o I²C. Motor
  de escova é ruído elétrico.

---

## 2. Servo (doseador) — GPIO10

```text
GPIO10 ──┬── 330 Ω ──────► sinal (fio laranja)
         └── 10 kΩ ── GND
buck 5 V ────────────────► V+ (fio vermelho), com 470 µF/16 V e 100 nF ao GND no conector
GND ─────────────────────► GND (fio marrom)
```

- O **10 kΩ ao GND** segura o sinal em baixo enquanto o GPIO ainda não foi
  configurado, no reset e no boot. Sem pulsos, o servo não se move, e o
  doseador não dá tranco toda vez que o módulo liga.
- O **330 Ω** limita a corrente se o fio de sinal encostar no V+ ou se o servo
  devolver ruído pelo sinal.
- O firmware liga o PWM só durante o movimento e solta depois. Um servo
  analógico sem pulsos não "caça" posição, e é essa caça parada que gasta o
  potenciômetro interno e as engrenagens. Se o slide precisar de força para
  ficar no lugar com M1 vibrando, segure o pulso só enquanto M1 roda.
- Ângulos de repouso e despejo calibrados na bancada, não fixos no firmware.

---

## 3. M1 e M2 (motores de vibração) — GPIO0 / GPIO1

```text
GPIO0 ── 1 kΩ ──┬── base                  (M2: GPIO1 ── 470 Ω ── base)
                │      [BC337]
               10 kΩ      coletor ──┬──── M1 ────┬── buck 5 V
                │                   ├── 1N5819 ──┘  (cátodo no 5 V)
               GND                  └── 100 nF em paralelo com o motor
                          emissor ── GND (retorno de potência, §1)
```

- **10 kΩ da base ao GND:** com o GPIO solto, no reset e no boot, o
  transistor fica cortado.
- **Base de 1 kΩ no M1 e de 470 Ω no M2:** o DC130P puxa mais na partida,
  e 470 Ω garantem a saturação do BC337 com folga, a ~5 mA do GPIO.
- **1N5819 em vez de 1N4148:** na hora de desligar, a corrente do motor passa
  inteira pelo diodo. O 1N4148 aguenta ~200 mA, menos que o pico do DC130P.
- **100 nF no motor:** corta o ruído das escovas, que de outro jeito chega ao
  I²C e ao enlace.
- **Onde fica cada peça.** O BC337, o resistor de base e o pull-down ficam na
  placa da base; o motor, no topo, a uns 30–50 cm de fio pela torre (§8). O
  100 nF e o 1N5819 vão **na ponta do motor** — no conector do topo, o mais
  perto possível dele: assim a corrente que circula no desligamento e o ruído
  das escovas ficam lá em cima, e não descem pela torre colados nos fios do
  sensor. Os fios de cada motor sobem trançados entre si.
- **M1 (1027) é motor de 3 V.** Vai do 5 V com PWM em ~60%, nunca 5 V
  contínuo: sobretensão gasta a escova. PWM a ~20 kHz, inaudível.
- **M2 (DC130P) entra com rampa de PWM:** movimento lento e gradual, nunca
  abrupto, porque vibração súbita no vidro estressa o betta.
- **M1** fica colado na parede do funil do reservatório e nunca toca água.
  Dispara pelo menos 1×/dia, mesmo sem alimentação completa.
- **M2** fica na ponta do tubo de queda e é o único ponto do módulo que encosta
  na água. Vede o motor com termorretrátil e silicone de cura neutra, faça uma
  alça de gotejamento no fio, e deixe o conector dentro da caixa, acima da
  linha d'água.

---

## 4. Sensor de contagem (IR + LM393) — GPIO3 e GPIO4

```text
GPIO4 ── 150 Ω ──► LED IR (ânodo) ── cátodo ── GND        (~14 mA, só na dosagem)

3V3 ── 10 kΩ ──┬── coletor do fototransistor ── emissor ── GND
               └──► entrada do LM393 ── saída do LM393 ──► GPIO3
LM393 alimentado em 3V3: a saída já sai em nível de 3,3 V
```

- LED e fototransistor em lados opostos do tubo, alinhados — feixe
  atravessando, não refletindo. Trecho do tubo fechado à luz externa.
- **O LED só acende durante a dosagem.** Dura mais, esquenta menos, e abre um
  **autoteste antes de cada refeição**: com o LED apagado, a saída precisa ler
  "bloqueado"; aceso, com o tubo vazio, "livre". Se uma das duas falhar —
  LED queimado, desalinhado, luz entrando, comparador desajustado, fio solto —,
  a refeição **sai do mesmo jeito, contada pelo servo**: um movimento do slide
  por grão pedido, sem repetir, e o `FED` vai com o motivo `SENSOR` (o app
  avisa). Foi a escolha do Arthur em 2026-10-02: o peixe não fica sem comer
  por causa de um sensor.
- **No topo, junto do tubo:** o LM393, o fototransistor e o pull-up de 10 kΩ.
  O sinal do fototransistor é fraco e de alta impedância — descer pela torre
  ao lado dos fios dos motores o encheria de ruído. Pela torre desce a saída
  do comparador, já digital. O resistor de 150 Ω do LED fica na base.
- O trimpot do LM393 se ajusta uma vez, com o LED aceso: nível limpo com o
  feixe livre e o oposto com um grão de teste. Depois, trave-o com verniz.
- O `GPIO3` é entrada do ADC1. Se um dia o comparador virar o elo fraco, o
  fototransistor pode ser lido direto, em analógico, com calibração por
  LED aceso/apagado, sem refazer a fiação.

---

## 5. RTC (DS3231) e display (SSD1306) — GPIO6 / GPIO7

Mesmo barramento I²C, endereços diferentes: `0x68` o RTC, `0x57` a EEPROM que
vem no módulo ZS-042, `0x3C` o display.

```text
3V3 ──┬──── VCC (DS3231)
      └──── VCC (SSD1306)

GPIO6 ┬──── SDA (DS3231)
      └──── SDA (SSD1306)

GPIO7 ┬──── SCL (DS3231)
      └──── SCL (SSD1306)

GND ──┴──── GND (os dois)
```

- **ZS-042 com CR2032: tire o caminho de carga** — o diodo 1N4148 ou o
  resistor de 200 Ω ao lado do suporte da bateria. O circuito foi feito para
  LIR2032 recarregável; "carregar" uma CR2032 a faz vazar ou estufar. Mesma
  nota do módulo principal (`pinagem-e-montagem-esp32.md`, §3).
- Fios de I²C curtos (até ~20 cm) e sem cruzar com os dos motores. Os pull-ups
  que vêm nos dois módulos bastam. Por isso o DS3231 e o display ficam **na
  base**, ao lado da Super Mini — o I²C não sobe pela torre. O firmware usa
  400 kHz.
- **Tela apagada por padrão** (desgaste do OLED): acende ao tocar no botão,
  fica 5 s e apaga, exceto no modo de programação (20 s de inatividade). O
  contraste fica abaixo do padrão, pelo mesmo motivo. Sem display, o módulo
  funciona igual: o firmware percebe na partida e segue com o LED e o console.

---

## 6. Botão físico — GPIO5

```text
GPIO5 ──┬── 1 kΩ ── (fio) ── botão ── GND
        └── 100 nF ── GND        (junto ao pino)
```

- `INPUT_PULLUP` com o nível **amostrado por timer** (2 ms; toque = 30 ms
  seguidos em baixo, soltura = 30 ms seguidos em alto), como o botão da luz no
  módulo principal. Não interrupção por borda: lá, a interrupção aceitou como
  toque os picos que o 1-Wire vizinho induzia no fio, e o relé trocava sozinho
  (UPGRADE/07). Aqui o servo e os motores são fontes de ruído piores.
- O **1 kΩ e o 100 nF** filtram o ruído que o fio do botão capta, seguram uma
  descarga eletrostática de quem toca o painel e limitam a corrente que o
  capacitor despeja nos contatos a cada toque.
- O significado do toque é do firmware: 1× acende e passa a página, 2×
  alimenta agora (dentro do limite de 3 refeições em 24 h), 3× testa o aviso
  ao peixe, segurar 2 s programa. Tabela completa no
  [README do firmware](../firmware/feeder-module/README.md#botão-e-tela).
- O botão fica **na base**, junto do display: apertar o topo empurraria o
  braço que está sobre o aquário.

---

## 7. Conector com o módulo principal (4 vias)

Na ordem física do conector montado no principal (a mesma tabela de
`pinagem-e-montagem-esp32.md`, §8):

| Pino | Cor | Sinal | Módulo principal (ESP32) | Este módulo (ESP32-C3) |
|---|---|---|---|---|
| 1 | preto | `GND` | `GND` comum | `GND`, no ponto de terra (§1) |
| 2 | verde | `12V` | `+12V` bruto da fonte | 1N5822 → PTC → buck (§1) |
| 3 | vermelho | `TX` → | `GPIO4` (`TXD2`) | 4,7 kΩ → `GPIO20` (RX) |
| 4 | azul | ← `RX` | `GPIO16` (`RXD2`) | `GPIO21` (TX) → 1 kΩ |

- **Por que os resistores nos fios de dados.** O módulo é ligado e desligado
  com o aquário funcionando. Se o 12 V ou um fio de dados encostar antes do
  GND, a corrente procura retorno pelos diodos de proteção dos pinos — dos dois
  chips. E, com o módulo desligado e o cabo no lugar, o TX do principal (3,3 V
  em repouso) alimentaria o C3 pelo pino de RX. O 4,7 kΩ limita isso a menos
  de 1 mA; o 1 kΩ faz o mesmo no outro sentido.
- **O principal detecta o módulo pelo pino 4** (firmware 2.0.1 do principal):
  ele mantém o fio em baixo com um pull-down, e a UART deste módulo, em
  repouso, o mantém em alto. Sem o módulo no fio, ou com ele desligado, o
  principal não manda nem `PING`, e o servidor não ouve falar de alimentador.
  Por isso o resistor do TX daqui fica em **no máximo 2,2 kΩ** (o pull-down de
  lá é de ~45 kΩ), e nada deste lado pode puxar o pino 4 para baixo.
- **Conector com chave** (não use Dupont solto) — inverter 12 V com GND aqui
  queima os dois módulos. Os dois lados do cabo crimpados com a mesma
  correspondência de cor e pino.
- **Os 12 V viajam brutos, de propósito:** com corrente menor no cabo, a queda
  é menor, e o buck local entrega 5 V limpos ali mesmo, longe do principal.
  Não mande 5 V pelo cabo.
- **Protocolo** (firmware 2.1.0 do principal, 1.0.0-beta deste): uma linha
  ASCII por mensagem, a 9600 baud. O principal manda `PING` (a cada 2 s, só
  com o módulo presente), `FEED [grãos] [FORCE]` e `CFG <h1> <h2> <grãos>
  <0|1>`. Este módulo responde `PONG <h1> <h2> <grãos> <auto> <idade_s> <req>
  <conf> <ok> <refeições_24h>`, empurra `SCHEDULE` a cada reconexão e manda
  `FED <req> <conf> <ok> <motivo> <origem>` depois de cada refeição e `DENIED
  <motivo> <refeições_24h> <origem>` quando recusa uma (limite de 24 h,
  ocupado, sem calibração, refeição perdida). Os dois lados descartam a linha
  inteira se um campo vier fora da faixa — referência completa em
  `firmware/bettacare/feeder_link.h` e no
  [README do firmware](../firmware/feeder-module/README.md#enlace-com-o-principal).
- O log de boot do ROM sai pelo `GPIO21` a 115200 baud a cada reset do C3 e
  chega ao principal como ruído a 9600. O parser de lá o descarta; não precisa
  suprimir. Depois do boot, o `GPIO21` é da UART1, e o console do sistema (a
  UART0) não chega mais ao fio.

---

## 8. Organização física: base, torre e topo

O módulo é uma peça em três partes. Desenho, medidas e o que o modelador 3D
precisa saber estão em [`modelagem-alimentador/`](./modelagem-alimentador/).

| Parte | O que leva | Por quê |
|---|---|---|
| **Base**, na mesa | Super Mini, DS3231, display, botão, buck, diodos, PTC, BC337 e resistores, conector do cabo do principal, jack de 12 V | Tudo o que não precisa estar no alto: a eletrônica longe da umidade da água, o I²C curto, o botão e a tela à mão |
| **Torre**, oca | Só os fios do topo | Leva o topo à altura da borda do aquário |
| **Topo** | Reservatório com o M1, slide e servo, tubo de queda com o par IR e o LM393, bico sobre a água com o M2 na ponta | Só o que precisa estar sobre a água |

- **Nada encosta no vidro nem na tampa do aquário.** O braço do topo passa por
  cima da borda com folga, e a única coisa do módulo que toca o aquário é a
  ponta do M2, na água — o aviso, de propósito. A vibração do M1, do servo e
  do resto do M2 não chega ao vidro, onde viraria um zumbido para o peixe.
- **Apoio:** sobre EVA macio colado sob a base, ou preso na lateral da mesa com
  coxins de borracha. Os dois seguram o que sobra de vibração longe da mesa
  — e, com ela, do móvel do aquário. Sobre EVA, a base precisa de lastro: o
  topo fica em balanço sobre o aquário.

**Fios pela torre** — doze, nenhum de I²C:

| Fios | De → para | Na ponta de cima |
|---|---|---|
| Servo: 5 V, GND, sinal | base → servo | 470 µF/16 V e 100 nF no conector (o pico de corrente sai dali, não do fio) |
| M1: 5 V e coletor | base → motor | 1N5819 e 100 nF; os dois fios trançados |
| M2: 5 V e coletor | base → motor | 1N5819 e 100 nF; trançados; o trecho até a ponta, vedado (§3) |
| LED IR: ânodo e cátodo | base (150 Ω) → LED | — |
| LM393: 3V3, GND e saída | base → comparador | o fototransistor e o pull-up de 10 kΩ junto dele |

O GND do LM393 e o cátodo do LED voltam ao terra de sinal da base; o do servo
e o 5 V dos motores, ao ponto de terra de potência (§1). Para o servo, fio de
24 AWG; o resto, 26 AWG.

- **Caixa da base acima da linha d'água** e fora da condensação, com furos de
  ventilação embaixo e nas laterais, nunca em cima (é por onde pinga). Um sachê
  de sílica-gel dentro, trocado quando saturar.
- **Placa perfurada com um conector JST-XH por periférico:** servo, M1, M2,
  LED IR, LM393, botão, I²C e o cabo do principal. Qualquer peça troca sem
  ferro de solda. Etiquete cada conector. No topo, os mesmos conectores numa
  plaquinha própria — o chicote da torre desconecta nas duas pontas.
- **A Super Mini em barra de pinos fêmea**, não soldada direto: o conector USB-C
  é o ponto mais frágil da placa (o desta já tem mau contato), e trocar a placa
  inteira custa pouco. O dia a dia é por OTA; o USB fica para recuperação.
- **Verniz acrílico** do lado da solda depois do roteiro de bancada, mascarando
  conectores, USB-C, trimpots e o botão BOOT.
- **Alívio de tração** em todo cabo que entra na caixa (prensa-cabo ou
  abraçadeira ancorada por dentro): é o puxão no cabo que solta a solda. O
  chicote da torre, preso nas duas pontas: tirar o topo para limpar não pode
  puxar fio.
- **Reservatório fechado e seco.** Ração úmida empelota, e é o que M1 existe
  para combater, mas não precisa ganhar ajuda.

---

## Roteiro de bancada

Teste o módulo **isolado**, sem o cabo de 4 vias no principal, antes de
integrar.

| # | Medição | Instrumento | O que decide |
|---|---|---|---|
| 1 | Saída do buck, antes de ligar qualquer coisa a jusante | Multímetro | **5,0 V ± 0,1 V**. Ajustável: acerte e trave o trimpot |
| 2 | Continuidade do conector de 4 vias, pino a pino, contra o lado do principal | Multímetro (continuidade) | Nenhuma inversão antes da primeira conexão |
| 3 | Só o jack ligado: tensão no pino 2 do conector do cabo | Multímetro | **~0 V**: o diodo impede o jack de alimentar o cabo |
| 4 | Ligar o módulo olhando o servo e os motores | Visual | Nenhum tranco no boot (os pull-downs funcionando) |
| 5 | `servo 40`, depois `calibrar` | Console | Repouso → despejo completo, sem travar, devagar |
| 6 | `teste sensor` e `teste feixe 20`, passando grãos à mão | Console | Autoteste ok; cada grão contado uma vez, nenhum em dobro |
| 7 | `teste m1` e `teste m2` | Visual/tátil | Vibração perceptível, partida suave no M2 |
| 8 | `estado` | Console | Relógio ok (DS3231), tela ok |
| 9 | USB-C ligado junto com o buck | Toque/termômetro | O 1N5819 não esquenta; a placa segue rodando |
| 10 | `teste grao`, dez vezes, com um copo sob o bico | Console | Dez em dez: um grão por dose, visto pelo sensor |

O console funciona pela USB ou pela rede, sem cabo (`POST /console`; ver o
[README do firmware](../firmware/feeder-module/README.md#depuração-pela-rede)).

**Faça a 1 e a 2 antes de qualquer outra coisa.** São as que evitam queimar um
componente por engano de fiação.

---

## Depois de montar

1. O firmware já está na placa (gravado por OTA em 2026-10-02); as
   atualizações seguem por `http://<IP>/update`, e o USB fica para
   recuperação.
2. Siga o roteiro de bancada com o módulo isolado — ele inclui a calibração.
3. Sem calibração gravada, nenhuma refeição sai, nem a da agenda: o módulo
   recusa (`CALIBRAR`), o LED da placa pisca e o app avisa.
4. Ligue o cabo de 4 vias no principal. No `/status` do principal,
   `feeder.present` vira verdadeiro em ~1 s, e `connected`, com o primeiro
   `PONG`. Tirando o cabo, os dois voltam a falso.
5. Só então passe o verniz e feche a caixa.

---

## O que o firmware faz com esta montagem

O que depende dos pinos e das peças daqui; o resto — agenda, limite de 24 h,
recuperação, botão, tela — no [README do firmware](../firmware/feeder-module/README.md).

- **Primeira coisa do `setup()`:** `GPIO0`, `1`, `4` e `10` como saída em
  nível baixo. Os pull-downs cobrem o reset; o firmware assume dali em diante.
- **Enlace:** `Serial1` (UART1) a 9600 baud em `GPIO20` (RX, com pull-up) e
  `GPIO21` (TX); console pelo USB nativo (`CDCOnBoot`) ou pela rede.
- **M1** em PWM a 60% e 20 kHz (motor de 3 V a partir de 5 V), com teto de 3 s
  por pulso; **M2** com rampa de 1 s, 2 s firme e rampa de 1 s, teto de 6 s.
- **LED IR** aceso só na dosagem, com o autoteste apagado/aceso antes de cada
  refeição; reprovado, a refeição sai contada pelo servo (`SENSOR`).
- **Servo:** 5° a cada 10 ms, sem tranco nem na calibração; PWM só durante o
  movimento, e solto depois que o slide assenta.
- **Botão** amostrado por timer (2 ms; 30 ms para toque e soltura).
- **OLED** a 400 kHz, apagado por padrão e com contraste reduzido; **LED da
  placa** (`GPIO8`, aceso em baixo): aceso na refeição, piscando com aviso.

---

Fontes das restrições do ESP32-C3:
[ESP-IDF — GPIO do ESP32-C3](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/peripherals/gpio.html),
[ESP32-C3 Wireless Adventure — pinos de *strapping*](https://espressif.github.io/esp32-c3-book-en/chapter_5/5.2/5.2.6.html)
e [ESP32-C3 SuperMini — pinos seguros](https://esp32.co.uk/esp32-c3-supermini-pinout-safe-gpios/).
