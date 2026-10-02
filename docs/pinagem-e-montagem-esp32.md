# Pinagem e Montagem — ESP32-WROOM-32D

Guia de montagem física do BettaCare. **Este é o documento a seguir com o ferro
de solda na mão.**

> **Revisado em 2026-08-21.** A versão anterior deste guia continha três erros
> de projeto que produziam falhas intermitentes e silenciosas — o resistor de
> 220 Ω na entrada do SSR, o PWM da ventoinha em push-pull, e o pull-up do
> 1-Wire tratado como opcional. O raciocínio completo, com as medições e as
> especificações que sustentam cada correção, está em
> [`UPGRADE/02-hardware-e-ligacoes.md`](../UPGRADE/02-hardware-e-ligacoes.md).
> Aqui fica só o "como montar".
>
> **Atualizado em 2026-10-01**, depois da montagem em bancada: corte de energia
> da ventoinha no `GPIO26` (o `GPIO27` queimou), botão lido por amostragem e
> não mais por interrupção (§2), enlace com o alimentador implementado do lado
> do ESP32 (§8) e o firmware de debug para investigar defeitos sem tirar o
> ESP32 da placa ("Depois de montar").

O módulo opcional de alimentação de precisão (rosca doseadora + sensor de
contagem) tem pinagem própria, num ESP32-C3 Super Mini separado — ver
[`pinagem-alimentador-modulo.md`](./pinagem-alimentador-modulo.md). Este
documento só trata dos dois pinos do lado do ESP32 que interligam os dois
módulos (seção 8).

---

## Aviso de segurança (AC)

O `SSR-40DA` comuta tensão de rede (`127 V / 220 V AC`).

- **Nunca** toque no lado AC energizado.
- Mantenha isolamento físico entre a baixa tensão (ESP32, sensores) e a rede AC.
- Interrompa **apenas a fase** da luminária no SSR — o neutro passa direto.
- Faça toda a montagem e todos os testes **com o lado AC desconectado**. Só
  energize a rede depois que o restante estiver validado.
- Para uma luminária de aquário (bem abaixo de 1 A num relé de 40 A) **não é
  preciso dissipador** no SSR.

---

## Pinagem

Nenhum destes pinos é de *strapping* (0, 2, 5, 12, 15), nenhum invade a faixa
reservada à flash (6–11), e o potenciômetro está no ADC1 — a única metade do
conversor que continua funcionando com o Wi-Fi ligado. **Esta atribuição está
correta e não deve ser alterada.**

| GPIO | Função | Observação |
|---|---|---|
| 18 | Push button | `INPUT_PULLUP`, amostrado a cada 2 ms (ver §2); botão para GND |
| 21 | SDA (DS3231) | I²C |
| 22 | SCL (DS3231) | I²C |
| 23 | Gate do MOSFET do SSR | Ver §4 — **sem** resistor em série com o SSR |
| 19 | DS18B20 DATA | 1-Wire, pull-up de 4,7 kΩ **obrigatório** |
| 34 | Potenciômetro (wiper) | ADC1, input-only, seguro com Wi-Fi |
| 17 | PWM da ventoinha (pino 4) | 25 kHz — ver §7, exige dreno aberto |
| 25 | Tacômetro (pino 3) | Pull-up externo de 4,7–10 kΩ para 3,3 V |
| 26 | Corte de energia da ventoinha | Ver §7 — MOSFET com **pullup** (não pulldown) no gate. **Era o `GPIO27`, que queimou em campo — não reutilize.** |

> **Interligação com o módulo do alimentador** — ver §8. `GPIO4` e `GPIO16` não
> conflitam com nenhum dos nove acima nem com *strapping*/flash. O lado do
> ESP32 principal já está implementado (`feeder_link.cpp`); o que falta é o
> firmware do próprio módulo.

| GPIO | Função | Observação |
|---|---|---|
| 4 | `TXD2` — para o módulo alimentador | UART2 a 9600 baud; envia `PING`, `FEED [grãos]` e `CFG <h1> <h2> <grãos> <auto>` |
| 16 | `RXD2` — do módulo alimentador | UART2, com o pull-up interno ligado pelo firmware; recebe `PONG`, `SCHEDULE` e `FED` |

---

## Lista de componentes

| Componente | Qtd. |
|---|---|
| ESP32 DevKit (WROOM-32D) | 1 |
| Módulo DS3231SN | 1 |
| Bateria CR2032 | 1 |
| SSR-40DA | 1 |
| Push button N.O. | 1 |
| DS18B20 (sonda de 3 fios, à prova d'água) | 1 |
| Potenciômetro B10K | 1 |
| Ventoinha 12 V de 4 pinos (conector padrão de CPU) | 1 |
| Fonte de 12 V | 1 |
| Conversor buck MP1584 ou LM2596 | 1 |
| MOSFET 2N7002 ou AO3400 | 3 |
| Diodo 1N4007 | 1 |
| Resistor 1 kΩ | 3 |
| Resistor 4,7 kΩ | 2 |
| Resistor 10 kΩ | 1 |
| Resistor 100 kΩ | 3 |
| Resistor 100 kΩ / 1 W | 1 |
| Capacitor cerâmico 100 nF (104) | 5 |
| Capacitor cerâmico 10 nF | 1 |
| Capacitor eletrolítico 10 µF / 25 V | 1 |
| Capacitor eletrolítico 470 µF / 16 V | 1 |
| Conector JST-XH 4 vias, com chave (macho + fêmea) | 1 par |

> **Sai da montagem antiga:** o resistor de 220 Ω entre o GPIO23 e o `SSR +`.

> O conector JST-XH só é necessário se o módulo do alimentador for montado —
> ver §8. Sem ele, o ESP32 funciona exatamente como hoje.

---

## 1. Alimentação

A lacuna mais séria da montagem anterior. O ESP32 consome picos de centenas de
miliampères durante a transmissão Wi-Fi, em rajadas de poucos milissegundos.
Um capacitor de 100 nF não sustenta rajada nenhuma — ele filtra ruído, não
entrega energia. Sem reservatório, cada transmissão puxa o trilho para baixo, e
se a queda cruzar o limiar do detector de brownout **o chip reinicia**.

Um ESP32 que reinicia durante a transmissão volta em segundos e reconecta,
deixando como único rastro um sistema que "ficou estranho por um instante".

### Fonte única (recomendado)

```text
Fonte 12V ──┬──────────────────────► +12V da ventoinha (pino 2)
            │
            └──► Buck (MP1584/LM2596) ──► 5V ──► VIN do ESP32
```

Elimina o cabo USB, elimina o segundo terra e elimina o laço de terra entre
eles. Se preferir manter duas fontes, o terra comum é **obrigatório**.

### Desacoplamento

```text
3V3 ──┬────────────┬──────────► DS3231, DS18B20, potenciômetro
      │            │
    470µF        100nF
      │            │
GND ──┴────────────┴──
```

- O **470 µF** vai o mais perto possível dos pinos do módulo ESP32.
- Um **100 nF** ao lado dele, e mais um junto de cada periférico (DS3231,
  DS18B20, wiper do potenciômetro).
- Cerâmico de 100 nF não tem polaridade; o eletrolítico tem — respeite a faixa.

### Terra em estrela

O retorno da ventoinha **não** deve compartilhar o mesmo trecho de fio que o
retorno dos sensores. Corrente de motor no caminho de terra do DS18B20 é ruído
injetado direto na medição de temperatura.

---

## 2. Push button (GPIO18)

```text
GPIO18 ──┬──── botão ──── GND
         │
       100nF
         │
        GND
```

- `GPIO18` → terminal A do botão
- Terminal B do botão → `GND`
- Capacitor de 100 nF entre `GPIO18` e `GND` (opcional; se usar, solde junto
  do pino do ESP32)

O firmware usa `INPUT_PULLUP` e um timer que **amostra o nível a cada 2 ms**: o
toque só conta depois de 30 ms seguidos em nível baixo, e o seguinte só depois
de 30 ms seguidos em nível alto. Um toque humano (80 ms ou mais) sempre passa;
um pico de microssegundos nunca soma 15 amostras seguidas.

> **Por que não interrupção na borda.** Era assim até 2026-10-01, e foi a causa
> do relé que mudava de estado sozinho a cada ~5 s (UPGRADE/07). O fio do
> `GPIO18` corre ao lado do `GPIO19`, o 1-Wire do DS18B20, e cada bit-slot do
> 1-Wire induz um pico de microssegundos no botão: medidos 16 picos no pedido
> de conversão e 152 na leitura, a cada 5 s — milhares por hora. A interrupção
> aceitava qualquer borda como toque, e o debounce de 50 ms não segurava nada
> porque o loop roda a cada 200 ms. Com a amostragem, os picos continuam
> chegando e não fazem diferença: em 32 min de teste, 62.909 picos e só as 4
> trocas dos toques reais.
>
> O 100 nF não resolve sozinho — com ele a 9 cm do pino, os picos chegavam do
> mesmo jeito. Afastar os fios do 18 e do 19 reduziria o acoplamento, mas não
> é necessário com o firmware atual. Para conferir o ruído na sua montagem, o
> firmware de debug conta os picos e as trocas (ver "Roteiro de bancada").

---

## 3. DS3231 (RTC, I²C)

- `GPIO21` → `SDA`
- `GPIO22` → `SCL`
- `3V3` → `VCC`
- `GND` → `GND`

Os módulos DS3231 comuns já trazem pull-up de I²C embarcado.

> ### ⚠️ Mantenha este módulo em 3,3 V
>
> Os módulos baratos (ZS-042 e parentes) trazem um circuito de recarga — diodo
> 1N4148 em série com resistor de 200 Ω — pensado para bateria **recarregável**
> LIR2032. Esta montagem usa **CR2032, que não é recarregável**.
>
> **Em 3,3 V isto é seguro**: a queda do diodo deixa menos de 2,7 V no lado da
> bateria, abaixo dos 3,0 V da CR2032, e nenhuma corrente de recarga flui.
>
> **Em 5 V, não.** Mover o `VCC` para 5 V — uma "correção" tentadora quando o
> I²C dá problema — faz a recarga acontecer, e uma CR2032 sendo carregada
> esquenta, vaza e pode romper. Se por qualquer motivo precisar ir a 5 V,
> **remova antes o resistor de 200 Ω (ou o diodo) da placa.**

Uma CR2032 dura anos. Se ela morrer em meses, o circuito de recarga é o
primeiro suspeito.

**Se o log mostrar `rtc.bad_read` ou `rtc.missing`.** O firmware confere cada
leitura (transação I²C e bytes da hora) e descarta a ruim em vez de usá-la.
Com três seguidas, dá o módulo como ausente, procura de novo a cada 15 s e,
enquanto isso, a automação da luz segue pela hora interna do ESP32, acertada
por NTP. Nada disso conserta a causa: confira `SDA`, `SCL`, as soldas e o
pull-up do módulo.

---

## 4. Luminária (SSR-40DA)

O SSR-40DA aceita **3 a 32 V DC** no controle e consome até **7,5 mA**. A faixa
larga existe porque a entrada já é limitada internamente — **o SSR já tem o
resistor de que precisa, dentro dele.** Foi por isso que o resistor de 220 Ω da
montagem anterior saiu: ele não protegia nada e só tirava margem.

### Opção A — Robusta (recomendada)

MOSFET no lado baixo, SSR alimentado com 5 V:

```text
5V (VIN da placa) ─────────────────────► SSR +

                                         SSR - ──┐
                                                 │
                                               dreno
GPIO23 ──── 1k ──── gate  [2N7002 / AO3400]
                      │                   fonte
                    100k                    │
                      │                     │
GND ──────────────────┴─────────────────────┘
```

O SSR vê **5 V** — meio da faixa, longe do joelho —, o ESP32 não fornece
corrente nenhuma, e o pulldown de 100 kΩ no gate garante desligado durante o
reset.

### Opção B — Mínima (custo zero)

GPIO23 direto no `SSR +`, sem resistor em série:

```text
GPIO23 ──┬──────────────► SSR +
         │
        10k
         │
GND ─────┴──────────────► SSR -
```

Os 7,5 mA ficam dentro dos 20 mA recomendados por pino do ESP32. Funciona, mas
entrega apenas 3,3 V — o limite inferior da faixa do SSR.

**Mantenha o pulldown de 10 kΩ nas duas opções.** É ele que segura o SSR
desligado enquanto o GPIO está em alta impedância, durante o reset.

### Lado AC

- Fase da rede → terminal 1 do SSR
- Terminal 2 do SSR → fase da luminária
- Neutro da rede → neutro da luminária (passa direto, **não** pelo SSR)

### Se a lâmpada brilhar fraco quando deveria estar apagada

O SSR-40DA é de cruzamento por zero e tem um snubber RC interno, que **vaza
alguns miliampères mesmo desligado**. Com lâmpada incandescente isso é
invisível; com driver de LED, pode acender fraco. Um resistor sangrador de
100 kΩ / 1 W em paralelo com a lâmpada resolve.

Não é defeito de montagem — é característica do componente. Vale saber antes de
sair procurando bug no firmware.

---

## 5. DS18B20 (temperatura, GPIO19)

```text
3V3 ──┬──── VCC (vermelho)
      │
     4k7
      │
GPIO19 ───┴──── DATA (amarelo)

GND ──────────── GND (preto)
```

- `GPIO19` → `DATA`
- `3V3` → `VCC`
- `GND` → `GND`
- **Resistor de 4,7 kΩ entre `DATA` e `3V3`** — o mais perto possível do ESP32
- Capacitor de 100 nF entre `VCC` e `GND`, junto do sensor

> ### O pull-up não é opcional
>
> A versão anterior deste guia dizia "se o sensor não tiver pull-up embutido,
> adicionar 4,7 kΩ". Esse "se" causava montagens sem o resistor.
>
> O 1-Wire **não funciona sem pull-up** — a linha é de dreno aberto e o
> resistor é quem gera o nível alto. As sondas DS18B20 à prova d'água de três
> fios, que são as usadas em aquário, **normalmente não trazem resistor
> nenhum**; quem traz é o módulo de placa.
>
> Sem ele o barramento pode até dar leituras — flutuando pela capacitância
> parasita, em condições favoráveis — e depois parar quando o cabo é movido ou
> a umidade muda. É o retrato de "o sensor sumiu".
>
> **Como verificar:** com o ESP32 desligado, meça a resistência entre `DATA` e
> `3V3`. Circuito aberto significa que o resistor não existe.

**Cabo longo** (mais de ~1 m até o aquário) pede **2,2 kΩ** em vez de 4,7 kΩ. A
capacitância do cabo arredonda as bordas, e baixar o resistor recupera o tempo
de subida.

**Use os três fios (VDD real). Não use modo parasita** — ele é frágil
justamente durante a conversão de temperatura, que é quando você precisa dele.

**Se o log mostrar `temp.reset_value`**, o sensor devolveu 85,0 °C, o valor
de power-on do registrador: ele reiniciou no meio da conversão. O firmware
descarta essa leitura (aceita, ela ligaria a ventoinha a 100%), mas a causa é
alimentação instável no sensor — confira o `VCC`, o `GND` e o 100 nF junto da
sonda.

---

## 6. Potenciômetro B10K (GPIO34)

```text
3V3 ────[lateral]   B10K   [lateral]──── GND
                 \         /
                  \ wiper /
                   GPIO34
                     │
                   100nF
                     │
                    GND
```

- Lateral 1 → `3V3`
- Lateral 2 → `GND`
- Wiper (pino central) → `GPIO34`
- Capacitor de 100 nF entre o wiper e `GND`

**Sentido de rotação:** para aumentar no sentido horário, deixe o `3V3` no lado
direito (olhando o eixo de frente). Se ficar invertido, troque apenas os dois
pinos laterais.

**Saturação do ADC.** O ADC do ESP32 é não linear perto dos extremos e satura
antes de 3,3 V. Gire o potenciômetro de ponta a ponta e confirme na aba
Diagnóstico do app que a leitura crua se move até o fim. Se saturar, um resistor
de ~1 kΩ entre o extremo do potenciômetro e o `3V3` mantém o wiper abaixo de
3,0 V.

---

## 7. Ventoinha 12 V de 4 pinos

### Ligações por pino do conector

| Pino | Fio | Vai para |
|---|---|---|
| 1 | Preto | `GND` da fonte 12 V (terra comum com o ESP32) |
| 2 | Verde | `+12 V` da fonte (sempre ligado) |
| 3 | Vermelho (Tach) | `GPIO25`, **com pull-up externo** — ver abaixo |
| 4 | Azul (PWM) | `GPIO17`, **em dreno aberto** — ver abaixo |

> **Cores do conector físico usado nesta montagem:** preto = `GND`, verde =
> `+12V`, vermelho = `Tach`, azul = `PWM`. Só os pinos 2 e 3 fogem do padrão
> da tabela acima (o conector comprado não trouxe o amarelo/verde de fábrica);
> pino 1 (preto/GND) e pino 4 (azul/PWM) já batem com a cor "de livro".

### Desacoplamento da linha de 12 V

```text
            +12V fonte
               │
               ├────[100nF]────┬────[10µF]──── GND
               │               │
               └───────────────┴──► Pino 2 (+12V)

GND fonte ────────────────────────► Pino 1 (GND)
       │
       └── terra comum ──► GND do ESP32
```

### Pino 4 (PWM) — exige dreno aberto

A norma da Intel (*4-Wire PWM Controlled Fans Specification*, rev. 1.3) é
explícita:

> §2.3.1 — The Hardware Monitor Device is required to provide an **open-drain
> or open-collector type output** for the PWM signal on pin 4.
>
> §2.4 — The trace from PWM output to the fan header **must not have a pull up
> or pull down**. The pull up is located in the fan hub.

Ou seja: **o pull-up mora dentro da ventoinha**, e o controlador só tem o
direito de puxar a linha para baixo. Uma saída push-pull força tensão contra
esse pull-up interno — e se a ventoinha puxar para 5 V, a corrente entra pelo
pino do ESP32 e é escoada pelo diodo de proteção. Não destrói na hora; é
estresse contínuo do pad, dia após dia.

**Antes de ligar, faça esta medição** (é o método da própria norma, §2.2):

> Alimente a ventoinha com 12 V, com o **ESP32 desconectado**, e meça a tensão
> contínua entre o **pino 4 e o pino 1**.

| Leitura | O que fazer |
|---|---|
| **~3,3 V** | Basta o GPIO17 em dreno aberto — correção só de firmware, sem componente |
| **~5 V** | Precisa de um MOSFET de buffer — **e a lógica do duty inverte** |

**Se der 5 V**, monte assim:

```text
Pino 4 da ventoinha ──── dreno
                                  [2N7002]
GPIO17 ──── 1k ──── gate
                      │            fonte
                    100k             │
                      │              │
GND ──────────────────┴──────────────┘
```

⚠️ **O MOSFET inverte o sinal.** Duty de 30 % no software vira 70 % na
ventoinha. Esta montagem mediu 5 V e usa o MOSFET, então `_apply_speed()` em
`fan.cpp` **já escreve o complemento**. Se um dia montar sem o MOSFET (caso dos
3,3 V), tire essa inversão do firmware — senão a ventoinha corre a 100 %
quando deveria estar parada, e para quando deveria correr.

### Pino 3 (Tacômetro) — pull-up externo obrigatório

A norma, §2.1.3: *dois pulsos por volta, saída open-collector ou open-drain,* e
a placa-mãe fornece o pull-up (a 12 V, num PC — o que aqui **destruiria** o
ESP32).

A montagem anterior usava só o pull-up interno do ESP32, de **~45 kΩ**. Com essa
resistência e a capacitância do cabo, a borda de subida fica lenta — e uma borda
lenta atravessando o limiar de comutação capta ruído, que está logo ali, na
chave de 25 kHz da própria ventoinha. Cada oscilação vira um pulso contado a
mais.

O resultado não é o sistema parar. É pior: RPM oscilando, e um firmware que
"vigia" o tacômetro chegando a conclusões erradas sobre uma ventoinha que está
girando perfeitamente.

```text
3V3 ──┬──
      │
     4k7 (ou 10k)
      │
GPIO25 ──┴──── 1k ──┬──── Pino 3 (Tach)
                    │
                  10nF
                    │
                   GND
```

- **Pull-up de 4,7 kΩ a 10 kΩ** entre `GPIO25` e `3V3` — obrigatório
- Filtro RC opcional de **1 kΩ + 10 nF**: corta acima de ~16 kHz e não encosta
  no sinal útil (a 2000 RPM são apenas 67 Hz, quatro ordens de grandeza abaixo)

### Duas notas de comportamento

**Se o sinal de PWM sumir, a ventoinha vai a 100 %** (norma, §3.3). Para um
aquário isso é a falha na direção certa: o pior caso do controlador é
resfriamento demais, nunca de menos. **Não anule essa proteção** adicionando um
pull-down externo no pino 4.

**"Duty 0 % desliga a ventoinha" não é universal.** A norma (§3.4) admite dois
comportamentos: no **Tipo A** a ventoinha continua girando na rotação mínima
para qualquer duty abaixo do mínimo, inclusive zero; só no **Tipo B** ela
desliga. Qual você tem é decisão do fabricante.

Isso importa porque interage com o alarme `fan.tach_stalled`: numa ventoinha
Tipo B cujo duty mínimo seja maior que os 30 % usados pelo firmware, a ventoinha
desliga sozinha e o sistema acusa defeito onde não há. **Confirme o tipo na
bancada** antes de confiar no alarme.

### Corte de energia (ventoinha Tipo A, que nunca desliga só pelo PWM)

Confirmado nesta montagem: é Tipo A — duty 0 % só leva à rotação mínima própria
dela, nunca para zero. Se "desligada" precisa significar parada de verdade, é
a alimentação física que tem que ser cortada, não só o sinal de PWM. Aqui o
corte é no **retorno**: o `+12V` fica sempre no pino 2, e um AO3400 de lado
baixo abre o caminho do pino 1 até o `GND`.

```text
+12V fonte ──┬──────────────────────────────────► Pino 2 da ventoinha (verde)
             │
           catodo [1N4007]
             │
            anodo
             │
Pino 1 da ventoinha (preto) ──┬────────────────── dreno [AO3400]
                              │
GPIO26 ──── 1k ──── gate      │
                      │       │
                    100k      fonte
                      │         │
                     3V3        │
                                │
GND comum ───────────────────────┘
```

- `+12V` continua sempre ligado no pino 2 — isso não muda.
- `1N4007`: catodo no `+12V`, anodo no mesmo nó do pino 1/dreno do AO3400 —
  protege contra o pico indutivo do motor no instante do corte.
- `GPIO26` → 1 kΩ → gate do AO3400, mesmo padrão dos outros MOSFETs deste
  documento.
- **O resistor de 100 kΩ aqui é pullup pro `3V3`, não pulldown pro `GND`** —
  o oposto do gate do SSR e do PWM. É deliberado: a regra do projeto é que uma
  falha sempre penda pra "mais resfriamento", nunca menos (mesmo raciocínio
  da nota anterior sobre o sinal de PWM sumir). Se o ESP32 reiniciar com o
  `GPIO26` em alta impedância, o pullup mantém o MOSFET ligado e a ventoinha
  energizada por padrão — só um comando explícito de "desligar" corta.

> **`GPIO27` era o pino original — queimou em campo** (UPGRADE/07). Causa mais
> provável: o AO3400 tem gate e dreno em pinos adjacentes no SOT-23, e um
> curto acidental de solda entre eles durante a montagem expôs o `GPIO27` a
> 12V através de só 1kΩ — tensão muito acima do que o pino aguenta. O resto do
> chip continuou saudável; só a saída daquele pino especificamente morreu.
> **Não reutilize o `GPIO27` pra nada neste projeto.** O firmware agora lê de
> volta o pino do corte (hoje o `GPIO26`) a cada 2 s e avisa uma vez por
> episódio se ele divergir do que foi escrito (`fan.power_pin_fault`, e
> `fan.power_pin_ok` quando volta) — é o retrato de pino queimado, pego na hora
> em vez de depois de dias de "a ventoinha não desliga".

---

## 8. Interligação com o módulo do alimentador (opcional)

O módulo de alimentação de precisão é fisicamente separado — RTC, botão e
display próprios, controlador próprio (ESP32-C3 Super Mini). Pinagem completa
dele em [`pinagem-alimentador-modulo.md`](./pinagem-alimentador-modulo.md);
aqui fica só o lado do ESP32.

Conector JST-XH de 4 vias, na **ordem física do conector usado nesta
montagem**:

| Pino | Cor | Sinal | Módulo principal (ESP32) | Módulo alimentador (ESP32-C3) |
|---|---|---|---|---|
| 1 | preto | `GND` | `GND` comum | `GND` |
| 2 | verde | `12V` | `+12V` bruto da fonte, **antes** do buck do ESP32 | entrada do regulador local de 5 V |
| 3 | vermelho | `TX` → | `GPIO4` (`TXD2`) | `GPIO5` (RX) |
| 4 | azul | ← `RX` | `GPIO16` (`RXD2`) | `GPIO4` (TX) |

Os `GPIO4`/`GPIO5` da última coluna são do ESP32-C3 — chip diferente do
`GPIO4` do módulo principal, numeração independente.

> **Sobre as cores:** não é a combinação "óbvia" (o costume seria vermelho no
> 12V), mas é a que veio no conector comprado. Os dois lados do cabo precisam
> ser crimpados com esta mesma correspondência de cor e pino — o conector é
> fêmea/macho e não força uma ordem única sozinho. Até 2026-10-01 o diagrama
> deste guia numerava `12V` como pino 1 e `GND` como pino 2; vale a ordem da
> tabela, que é a do conector montado.

- Por que `GPIO4`/`GPIO16` e não os pinos "padrão" da UART2 (`GPIO17`/`GPIO16`)?
  `GPIO17` já é o PWM da ventoinha (§7) — trocar de pino evita reabrir uma
  ligação já validada na bancada.
- Por que UART e não estender o I²C do RTC (`GPIO21`/`22`, já existe)? I²C não
  foi pensado pra atravessar um conector destacável — a capacitância do cabo
  prejudica a integridade do sinal nesse barramento. Serial tolera isso bem
  melhor e usa a mesma quantidade de fios.
- **Sem o cabo conectado, o alimentador continua funcionando sozinho** (RTC e
  agenda próprios) — a integração só existe pra centralizar agenda, health-check
  e histórico no servidor/PWA. Desse lado já está tudo pronto: o enlace no
  firmware do ESP32 principal, o servidor e o app. Sem o módulo, o ESP32 manda
  um `PING` a cada 2 s e simplesmente reporta "desconectado", que é o estado
  normal.
- O enlace não tem checksum, e o `GPIO16` corre ao lado do PWM de 25 kHz no
  `GPIO17`. Por isso o firmware liga o pull-up interno no RX (com o conector
  vazio, o fio vira antena) e descarta qualquer linha com campo fora da faixa
  do contrato — o próximo `PONG`, 2 s depois, traz o valor certo.

---

## Roteiro de bancada

Faça tudo **com o ESP32 no USB, fora do aquário, e com o lado AC
desconectado**. Se o firmware novo quebrar a rede, o OTA não salva.

| # | Medição | Instrumento | O que decide |
|---|---|---|---|
| 1 | Tensão `SSR +` → `SSR -` com a luz comandada ligada | Multímetro | Precisa ficar **acima de 3 V** (5 V na opção A) |
| 2 | Tensão pino 4 → pino 1 da ventoinha, **ESP32 desconectado** | Multímetro | 3,3 V → só firmware; 5 V → precisa do MOSFET |
| 3 | Resistência `DATA` → `3V3` do DS18B20, sem energia | Multímetro | Precisa ler ~4,7 kΩ. Circuito aberto = falta o pull-up |
| 4 | Forma de onda no GPIO17 com duty a 50 % | Osciloscópio | Confirma o dreno aberto |
| 5 | Forma de onda no GPIO25 com a ventoinha girando | Osciloscópio | Bordas limpas, sem ruído no cruzamento |
| 6 | `3V3` durante transmissão Wi-Fi | Osciloscópio, acoplamento AC | Quedas devem ficar **abaixo de 200 mV** |
| 7 | Curso completo do potenciômetro | Aba Diagnóstico do app | A leitura crua precisa varrer até o fim |
| 8 | Motivo do último reset, ao longo de 48 h | Aba Diagnóstico do app | `brownout` = alimentação; `task_wdt` = software |
| 9 | Continuidade do conector de 4 vias do alimentador, pino a pino, **antes de plugar** | Multímetro (continuidade) | Só aplica se o módulo do alimentador estiver sendo integrado — ver §8 |
| 10 | Picos no `GPIO18` × trocas da luz, por alguns minutos, sem tocar no botão | Firmware de debug, endpoint `/debug` | Picos são esperados (acoplamento do 1-Wire, §2); trocas da luz pelo botão têm de ficar em zero |

**Se você só fizer três medições, faça as de número 1, 2 e 3.** São multímetro
e cobrem as três interfaces mais críticas.

A medição 8 é a que fecha o ciclo: o firmware agora reporta
`esp_reset_reason()` na telemetria, e é isso que transforma a suspeita de
brownout em fato — ou a descarta.

---

## Depois de montar

1. `cp config.example.h config.h` e preencha `WIFI_SSID`, `WIFI_PASSWORD`,
   `SERVER_HOST`, `SERVER_API_TOKEN`, `API_AUTH_TOKEN` e `OTA_PASSWORD`.
2. **Rotacione a senha de OTA.** A do repositório antigo esteve versionada em
   texto claro e deve ser considerada comprometida.
3. Grave, e rode o **diagnóstico geral** pela aba Diagnóstico do app: ele varre
   o barramento 1-Wire, sonda o I²C, testa a NVS, lê o ADC de ponta a ponta e
   devolve um relatório componente a componente.
4. Só depois de o diagnóstico passar limpo, energize o lado AC e instale no
   aquário.

**Se algo se comportar estranho depois de montado**, antes de ressoldar,
grave o firmware de debug (`firmware/bettacare-debug`, por OTA — o ESP32 não
precisa sair da placa). Ele expõe `/debug`, que liga e desliga cada subsistema
e injeta falhas sob comando, e separa o que é firmware do que é montagem com
um experimento de minutos. Foi assim que o relé que piscava sozinho foi
resolvido, sem trocar componente nenhum. Instruções no `README.md` daquela
pasta; depois, grave de volta o de produção.
