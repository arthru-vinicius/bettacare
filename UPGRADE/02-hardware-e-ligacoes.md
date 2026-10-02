# Hardware, ligações e componentes

Auditoria elétrica da montagem descrita em `docs/pinagem-e-montagem-esp32.md`,
confrontada com o que o firmware realmente configura nos pinos e com as
especificações dos componentes.

**A tese deste documento em uma frase:** três das quatro interfaces físicas do
sistema estão fora da especificação do componente que elas controlam, e as três
falham do mesmo jeito — de forma intermitente, dependente de temperatura e de
lote, sem deixar rastro no software. É o retrato exato da queixa que motivou
esta rodada.

Nenhum trabalho de observabilidade no firmware corrige isto. Instrumentação boa
faz o defeito **aparecer**; ela não o remove.

---

## 1. Veredito por interface

| Interface | GPIO | Situação | Gravidade |
|---|---|---|---|
| Luminária (SSR-40DA) | 23 | **Fora de especificação** — resistor de 220 Ω derruba a tensão de controle abaixo do mínimo | **Crítica** |
| Ventoinha PWM (pino 4) | 17 | **Fora de especificação** — saída push-pull onde a norma exige dreno aberto | **Alta** |
| Tacômetro (pino 3) | 25 | **Marginal** — só o pull-up interno de ~45 kΩ | **Média** |
| DS18B20 (1-Wire) | 19 | **Indeterminado** — o pull-up de 4,7 kΩ está descrito como condicional | **Média** |
| Potenciômetro | 34 | **Correto** — ADC1, funciona com Wi-Fi ligado | — |
| Botão | 18 | **Correto** | — |
| DS3231 (I²C) | 21/22 | **Correto**, com uma ressalva sobre a bateria | — |
| Alimentação | — | **Não especificada** — sem capacitância de reservatório | **Alta** |

A escolha dos pinos, vale registrar, está **certa** e não deve ser mexida:
nenhum deles é pino de strapping (0, 2, 5, 12, 15), nenhum invade a faixa da
flash (6–11), e o potenciômetro está no GPIO34, que é ADC1 — a única metade do
conversor que continua funcionando com o rádio ligado. Quem montou isto sabia o
que estava fazendo. Os problemas estão nos componentes discretos ao redor dos
pinos, não na atribuição deles.

---

## 2. A luminária: o resistor de 220 Ω é o defeito

Este é o achado mais provável de explicar *"às vezes ele simplesmente não
funcionava"*.

### O que está montado

```text
GPIO23 ----+---- 220R ---- SSR +
           |
          10k
           |
GND -------+-------------- SSR -
```

### Por que não fecha a conta

O SSR-40DA aceita **3 a 32 V DC** no controle e consome **até 7,5 mA**. A faixa
larga existe porque a entrada já é limitada internamente — é isso que permite
ligar o mesmo módulo em 5 V, 12 V ou 24 V sem trocar nada. **O SSR já tem o
resistor que ele precisa, dentro dele.**

Com 220 Ω em série e 3,3 V na origem:

| Corrente | Queda no resistor | Sobra para o SSR |
|---|---|---|
| 7,5 mA | 1,65 V | **1,65 V** |
| 5,0 mA | 1,10 V | 2,20 V |
| 3,0 mA | 0,66 V | 2,64 V |

Em nenhuma linha da tabela o SSR recebe os 3 V do mínimo. O que se monta assim
é um circuito que opera **no joelho da curva do LED interno** — e o joelho de um
LED anda com a temperatura. Num dia frio liga; com o dissipador aquecido, ou com
outro lote do módulo, não liga. Nunca dá erro, porque do lado do ESP32 está tudo
perfeito: o GPIO foi para nível alto, o firmware registrou `light.on = true`, e
a lâmpada continua apagada.

É exatamente o cenário que a seção 3 de `docs/arquitetura-observabilidade.md`
descreve como luminária em `fault` — "estado desejado ≠ estado reportado". Só
que o firmware **reporta o que ele comandou**, não o que a lâmpada fez. O
diagnóstico do documento nunca dispara, porque o firmware acredita nele mesmo.

### As duas correções

**Correção mínima (custo zero):** remover o resistor de 220 Ω, ligando o GPIO23
direto no `SSR +`. O consumo de 7,5 mA está confortavelmente dentro dos 20 mA
recomendados por pino do ESP32. Mantenha o pulldown de 10 kΩ — ele é útil e não
atrapalha: segura o SSR desligado durante o reset, quando o GPIO fica em alta
impedância.

Isto entrega 3,3 V ao SSR. Funciona, mas ainda é o limite inferior da faixa.

**Correção robusta (recomendada):** comutar o lado baixo com um MOSFET de sinal
e alimentar o SSR com 5 V.

```text
5V (VIN da placa) ------------------- SSR +

                                      SSR - ----+
                                                |
                                              dreno
GPIO23 ---- 1k ---- gate  [2N7002 ou AO3400]
                     |                  fonte
                    100k                  |
                     |                    |
GND -----------------+--------------------+
```

O SSR passa a ver 5 V — meio da faixa, longe do joelho —, o ESP32 não fornece
corrente nenhuma, e o pulldown de 100 kΩ no gate garante desligado no boot. Um
2N7002 custa centavos e resolve a classe inteira do problema.

### Como confirmar antes de mexer

Com a luminária comandada para ligar, meça a tensão **contínua entre `SSR +` e
`SSR -`**. Abaixo de 3 V, o defeito está confirmado. É um teste de trinta
segundos com um multímetro e vale mais que qualquer inspeção de código.

### Ressalva do lado AC

O SSR-40DA é de cruzamento por zero e tem um snubber RC interno, que **vaza
alguns miliampères mesmo desligado**. Com lâmpada incandescente isso é
invisível; com driver de LED, a lâmpada pode brilhar fraco quando deveria estar
apagada. Se isso aparecer, um resistor sangrador de 100 kΩ / 1 W em paralelo com
a lâmpada resolve. Não é defeito do projeto, é característica do componente —
mas é bom saber antes de sair procurando bug no firmware.

Para uma luminária de aquário (bem abaixo de 1 A num relé de 40 A) **não é
preciso dissipador**.

---

## 3. A ventoinha: push-pull onde a norma pede dreno aberto

### O que a especificação exige

A norma da Intel para ventoinhas PWM de 4 fios (*4-Wire Pulse Width Modulation
Controlled Fans Specification*, rev. 1.3) é explícita nos dois pontos que
importam aqui.

Seção 2.3.1, sobre o que o **controlador** deve fornecer:

> The Hardware Monitor Device is required to provide an **open-drain or
> open-collector type output** for the PWM signal on pin 4.
> Current sink capability: 5 mA required, 8 mA recommended.
> Maximum voltage capability: 5.25 V.

Seção 2.1.4, sobre o lado da ventoinha:

> Absolute maximum voltage level: VMax = 5.25 V (open circuit voltage).
> **This signal must be pulled up to a maximum of 5.25 V within the fan.**
> *Note:* New fan designs are strongly encouraged to implement a 3.3 V pull up.

E a seção 2.4, que é a mais direta:

> The trace from PWM output to the fan header **must not have a pull up or pull
> down**. The pull up is located in the fan hub. The presence of a pull up on
> the motherboard will alter the fan response to the PWM Duty Cycle.

Ou seja: **o pull-up mora dentro da ventoinha**, e o controlador só tem o
direito de puxar a linha para baixo.

### O que o firmware faz

`firmware/bettacare/fan.cpp:105` chama `ledcAttach(PIN_FAN, ...)`, e essa função
configura o pino como saída **push-pull** comum. Não há nenhuma chamada a
`pinMode` com `OPEN_DRAIN` no arquivo. O ESP32 está, portanto, forçando 3,3 V
contra um pull-up que a norma permite estar em 5,25 V.

Duas consequências, ambas ruins e nenhuma visível em software:

1. **Corrente entrando no pino.** Quando o ESP32 tenta impor 3,3 V num nó que a
   ventoinha puxa para 5 V, a corrente entra pelo pino e é escoada pelo diodo de
   proteção do ESD para o trilho de 3,3 V. A norma limita essa corrente a 5 mA
   de curto-circuito, então não é destruição imediata — é **estresse contínuo do
   pad**, dia após dia. É assim que um GPIO "para de funcionar do nada" depois
   de meses.
2. **Duty cycle deformado.** É literalmente o que a seção 2.4 avisa: a resposta
   da ventoinha deixa de acompanhar o duty comandado. A escala de 30/55/80/100
   do firmware não significa o que ela diz que significa.

### O teste que decide a correção

A própria norma dá o método (seção 2.2): **alimente a ventoinha com 12 V, com o
ESP32 desconectado, e meça a tensão entre o pino 4 e o pino 1.**

| Leitura | Correção |
|---|---|
| ~3,3 V | Basta pôr o GPIO17 em dreno aberto — **custo zero, só firmware** |
| ~5 V | Precisa de um MOSFET de buffer — **e a lógica do duty inverte** |

**Se der 3,3 V:** configurar o pino como dreno aberto. Há uma armadilha
conhecida do core Arduino-ESP32 aqui — `ledcAttach` chama `pinMode(pin, OUTPUT)`
internamente e sobrescreve o modo, e a constante `OUTPUT_OPEN_DRAIN` teve
historicamente comportamento errado, funcionando só na forma `OUTPUT |
OPEN_DRAIN`. A ordem correta é aplicar o modo **depois** do `ledcAttach`, e
**conferir no osciloscópio** que a forma de onda tem os 3,3 V vindos da
ventoinha e não do ESP32. Não aceite este ponto sem medir.

**Se der 5 V:** um 2N7002 no lado baixo, dreno no pino 4 da ventoinha, fonte no
GND, gate no GPIO17. O ESP32 deixa de ver os 5 V. **Atenção:** o MOSFET inverte
o sinal — duty de 30 % no software vira 70 % na ventoinha. `_apply_speed()` em
`fan.cpp:69` precisa passar a escrever o complemento, e esse é o tipo de
inversão que, esquecida, faz a ventoinha correr a 100 % quando deveria estar
parada.

### Um detalhe da norma que serve de rede de segurança

Seção 3.3:

> If no control signal is present the fan shall operate at maximum RPM.

Se o ESP32 morrer, a ventoinha vai para velocidade máxima. Para um aquário isso
é a falha na direção certa: o pior caso do controlador é resfriamento demais,
nunca de menos. Vale saber que essa proteção existe — e vale **não** anulá-la
por acidente ao adicionar um pull-down externo.

### E um que contradiz a documentação atual

`docs/pinagem-e-montagem-esp32.md` afirma que "duty 0 % desliga a fan". A norma
(seção 3.4) admite **dois** comportamentos: na implementação Tipo A a ventoinha
continua girando na rotação mínima para qualquer duty abaixo do mínimo,
inclusive zero; só na Tipo B ela desliga. Qual delas você tem é decisão do
fabricante.

Isso importa porque interage com o diagnóstico de `fan.tach_stalled`: numa
ventoinha Tipo A, duty 0 com RPM > 0 é o comportamento **correto**, e qualquer
regra que trate isso como anomalia vai gerar alarme falso. Confirme o tipo na
bancada antes de fixar a regra.

---

## 4. O tacômetro: pull-up fraco demais

A norma, seção 2.1.3:

> Two pulses per revolution.
> **Open-collector or open-drain type output.**
> Motherboard will have a pull up to 12 V, maximum 12.6 V.

Duas leituras deste trecho:

**A boa notícia:** dois pulsos por volta, que é o que o firmware assume. Correto.

**O problema:** a norma espera que **o host** forneça o pull-up, e numa placa-mãe
ele vai a 12 V justamente para ter margem de ruído. Aqui não se pode fazer isso
— 12 V num GPIO destrói o ESP32 na hora. Mas o que está montado é o outro
extremo: `fan.cpp:114` usa apenas `INPUT_PULLUP`, o pull-up interno do ESP32, de
**~45 kΩ**.

Com 45 kΩ e a capacitância de um cabo de ventoinha, a borda de subida fica
lenta. Uma borda lenta atravessando o limiar de comutação é uma borda que capta
ruído — e o ruído está logo ali, na chave de 25 kHz da própria ventoinha, no
cabo vizinho. A interrupção está armada em `FALLING` (`fan.cpp:115`), então cada
oscilação na travessia vira **um pulso contado a mais**.

O resultado não é o sistema parar. É pior: RPM oscilando, e um firmware que
"vigia" o tacômetro chegando a conclusões erradas sobre uma ventoinha que está
girando perfeitamente.

**Correção:** um pull-up externo de **4,7 kΩ a 10 kΩ para 3,3 V** no GPIO25.
Opcionalmente, um filtro RC de 1 kΩ + 10 nF, que corta acima de ~16 kHz e não
encosta no sinal útil — a 2000 RPM são apenas 67 Hz, quatro ordens de grandeza
abaixo. Custa dois componentes e transforma a medição.

No firmware, o complemento natural é rejeitar na ISR intervalos mais curtos que
o fisicamente possível. Mas o filtro certo é o de hardware; software não
conserta borda ruim.

---

## 5. O DS18B20: o pull-up condicional

`docs/pinagem-e-montagem-esp32.md` diz:

> Se o sensor não tiver pull-up embutido: adicionar 4,7k entre DATA e 3V3.

Este "se" precisa virar um fato verificado. O 1-Wire **não funciona sem
pull-up** — a linha é de dreno aberto e o resistor é quem gera o nível alto. E
as sondas DS18B20 à prova d'água de três fios, que são as usadas em aquário,
**normalmente não trazem resistor nenhum**; quem traz é o módulo de placa. Se a
sua é a sonda de cabo, o resistor é obrigatório.

Um sistema montado sem ele pode até dar leituras — flutuando pela capacitância
parasita, em condições favoráveis — e depois parar quando o cabo é movido ou a
umidade muda. É o retrato de *"o sensor sumiu"*.

**Ações:**

1. **Meça.** Com o ESP32 desligado, resistência entre `DATA` e `3V3`. Se der
   circuito aberto, o resistor não existe.
2. **Instale 4,7 kΩ** entre `DATA` e `3V3`, o mais perto possível do ESP32.
3. **Cabo longo** (mais de ~1 m até o aquário) pede **2,2 kΩ**, não 4,7 kΩ. A
   capacitância do cabo arredonda as bordas, e baixar o resistor recupera o
   tempo de subida.
4. Mantenha a alimentação em três fios (VDD real). **Não use modo parasita** —
   ele é sensível justamente durante a conversão de temperatura, que é quando
   você precisa dele.

O firmware já faz a parte dele: `docs/firmware.md` registra a re-sondagem do
barramento a cada 10 s, então um sensor que cair depois do boot é detectado.
Isso é bom e deve ficar. Mas detectar melhor não substitui não cair.

---

## 6. Alimentação: o suspeito de "às vezes ele simplesmente não funcionava"

A montagem documentada especifica capacitores de desacoplamento de 100 nF, e
**nenhum capacitor de reservatório**. Isso é uma lacuna séria num projeto com
rádio.

O ESP32 consome picos de centenas de miliampères durante a transmissão Wi-Fi,
em rajadas de poucos milissegundos. Um capacitor de 100 nF não sustenta rajada
nenhuma — ele existe para ruído de alta frequência, não para energia. Quem
sustenta é o eletrolítico, e ele não está na lista.

Sem ele, cada transmissão puxa o trilho para baixo. Se a queda cruzar o limiar
do detector de brownout, o chip **reinicia**. E um ESP32 que reinicia durante a
transmissão volta em poucos segundos, reconecta e continua — deixando como único
rastro um sistema que "ficou estranho por um instante". Com o firmware antigo,
sem logs, isso era rigorosamente invisível.

Agravantes prováveis nesta montagem: alimentação por USB com cabo fino (a queda
está no cabo, não na fonte), regulador AMS1117 da placa de desenvolvimento com
pouca margem, e uma fonte de 12 V chaveada da ventoinha compartilhando terra.

**Correções, em ordem de retorno:**

1. **470 µF eletrolítico + 100 nF cerâmico** entre `3V3` e `GND`, o mais perto
   possível dos pinos do módulo. É o item de maior retorno por real de todo
   este documento.
2. **Uma fonte só.** 12 V para a ventoinha, e um conversor *buck* (MP1584 ou
   LM2596) tirando 5 V dela para o `VIN` do ESP32. Elimina o USB, elimina o
   segundo terra e elimina o laço de terra entre eles. Se preferir manter duas
   fontes, o terra comum já está previsto e é obrigatório.
3. **Terra em estrela.** O retorno da ventoinha não deve passar pelo mesmo
   trecho de fio que o retorno dos sensores. Corrente de motor no caminho de
   terra do DS18B20 é ruído injetado direto na medição.

### Como confirmar

Este é o ponto onde hardware e firmware se encontram, e é a razão de as duas
frentes andarem juntas: **`esp_reset_reason()` reportado na telemetria responde
esta pergunta de forma definitiva.** Se aparecerem reinícios por brownout, a
hipótese está provada e a correção é a de cima. Se aparecerem por *task
watchdog*, o problema é software e mora em outro documento.

Sem essa instrumentação, a discussão sobre alimentação é opinião. Com ela, é
medição. Vale priorizar o campo `reset_reason` acima de quase todo o resto do
plano de telemetria.

---

## 7. O DS3231 e a bateria

A montagem está correta: I²C em 21/22, alimentação em 3,3 V, pull-ups já
presentes no módulo. Uma única ressalva, e ela é preventiva.

Os módulos DS3231 baratos (ZS-042 e parentes) trazem um circuito de recarga —
um diodo 1N4148 em série com um resistor de 200 Ω — pensado para bateria
recarregável LIR2032. A montagem usa **CR2032, que não é recarregável**.

**Em 3,3 V isto é seguro:** a queda do diodo (~0,6 V) deixa menos de 2,7 V no
lado da bateria, abaixo dos 3,0 V da CR2032, e nenhuma corrente de recarga flui.
Não há nada a fazer hoje.

**O risco é futuro e é real:** se alguém algum dia mover o `VCC` do módulo para
5 V — e essa é uma "correção" tentadora quando o I²C dá problema — a recarga
passa a acontecer, e uma CR2032 sendo carregada esquenta, vaza e pode romper.

**Registre a proibição:** este módulo fica em 3,3 V. Se por qualquer motivo
precisar ir a 5 V, remova antes o resistor de 200 Ω (ou o diodo) da placa.

Vale ainda saber que `lostPower` do DS3231 é justamente o sintoma de bateria
morta, e o documento de observabilidade já o classifica como `degraded`. Uma
CR2032 dura anos; se ela morrer em meses, o circuito de recarga é o primeiro
suspeito.

---

## 8. O potenciômetro e a faixa do ADC

O pino está certo — GPIO34 é ADC1, e ADC1 é a metade do conversor que sobrevive
ao Wi-Fi ligado. ADC2 seria inutilizável aqui, e o projeto acertou.

O ponto de atenção é outro: `fan.cpp:215` chama `analogRead(PIN_POT)` sem
configurar a atenuação. O padrão do core é a atenuação máxima, cuja faixa
nominal vai até ~3,1 V — ou seja, **o topo do curso do potenciômetro satura**,
e os últimos graus de rotação não mudam a leitura. O ADC do ESP32 também é
notoriamente não linear perto dos dois extremos.

Como o firmware faz calibração do potenciômetro, é possível que isto já esteja
absorvido na prática. Fica registrado como ponto a verificar em bancada: gire o
potenciômetro de ponta a ponta e confirme que a leitura crua se move até o fim.
Se saturar, a saída mais simples é elétrica — um resistor de ~1 kΩ entre o
extremo do potenciômetro e o `3V3`, mantendo o wiper abaixo de 3,0 V.

---

## 9. Lista de compras

Tudo o que este documento acrescenta à montagem atual. Nada aqui é caro nem
difícil de achar.

| Item | Quantidade | Para quê | Prioridade |
|---|---|---|---|
| Eletrolítico 470 µF / 16 V | 1 | Reservatório no `3V3` — brownout | **Alta** |
| MOSFET 2N7002 ou AO3400 | 1–2 | Driver do SSR; buffer do PWM se a ventoinha puxar 5 V | **Alta** |
| Resistor 100 kΩ | 1–2 | Pulldown de gate dos MOSFETs | **Alta** |
| Resistor 4,7 kΩ | 1 | Pull-up do 1-Wire (2,2 kΩ se o cabo for longo) | **Alta** |
| Resistor 4,7 kΩ ou 10 kΩ | 1 | Pull-up do tacômetro | Média |
| Resistor 1 kΩ | 2 | Gate do MOSFET; filtro do tacômetro | Média |
| Capacitor 10 nF | 1 | Filtro do tacômetro | Média |
| Conversor buck MP1584 ou LM2596 | 1 | 12 V → 5 V, fonte única | Média |
| Resistor 100 kΩ / 1 W | 1 | Sangrador do SSR, se a lâmpada brilhar apagada | Baixa |

**Sai da montagem:** o resistor de 220 Ω entre o GPIO23 e o `SSR +`.

---

## 10. Roteiro de bancada

Ordem pensada para que cada passo produza uma resposta antes de custar
componente. **Faça tudo com o ESP32 no USB, fora do aquário.** Se o firmware
novo derrubar a rede, o OTA não salva — o aviso já está em `docs/firmware.md` e
continua valendo.

| # | Medição | Instrumento | O que decide |
|---|---|---|---|
| 1 | Tensão `SSR +` → `SSR -` com a luz comandada ligada | Multímetro | < 3 V confirma o defeito da luminária |
| 2 | Tensão pino 4 → pino 1 da ventoinha, ESP32 desconectado | Multímetro | 3,3 V → correção só em firmware; 5 V → precisa do MOSFET |
| 3 | Resistência `DATA` → `3V3` do DS18B20, sem energia | Multímetro | Circuito aberto = falta o pull-up |
| 4 | Forma de onda no GPIO17 com duty a 50 % | Osciloscópio | Confirma o dreno aberto depois da correção |
| 5 | Forma de onda no GPIO25 com a ventoinha girando | Osciloscópio | Bordas limpas vs. ruído no cruzamento |
| 6 | `3V3` durante transmissão Wi-Fi | Osciloscópio, AC | Quedas > 200 mV pedem o eletrolítico |
| 7 | Curso completo do potenciômetro, ADC cru | Firmware | Saturação no topo |
| 8 | `esp_reset_reason()` ao longo de 48 h | Telemetria | Brownout (hardware) vs. watchdog (software) |

Os itens 1, 2 e 3 são multímetro e resolvem as três interfaces mais graves. Se
você só fizer três medições, faça essas.

---

## 11. O que este documento não muda

Vale ser explícito sobre o que está certo, para ninguém "corrigir" por engano
numa próxima passada:

- **A atribuição dos pinos.** Nenhum strapping, nada na faixa da flash, ADC1
  para o potenciômetro. Não mexa.
- **O terra comum** entre a fonte de 12 V e o ESP32. Já está previsto e é
  obrigatório.
- **O pulldown de 10 kΩ no GPIO23.** Segura o SSR desligado durante o reset.
  Mantenha, inclusive depois de tirar o resistor de 220 Ω.
- **A alimentação do DS18B20 em três fios.** Modo parasita seria pior.
- **O isolamento físico entre o lado AC e a baixa tensão**, e a interrupção
  apenas da fase pelo SSR. O aviso de segurança de
  `docs/pinagem-e-montagem-esp32.md` continua integralmente válido.

---

## Fontes

- [4-Wire Pulse Width Modulation (PWM) Controlled Fans Specification, rev. 1.3 — Intel](https://glkinst.com/cables/cable_pics/4_Wire_PWM_Spec.pdf) — seções 2.1.3, 2.1.4, 2.2, 2.3.1, 2.4 e 3.3
- [SSR-40DA — especificações de entrada](https://docs.cirkitdesigner.com/component/b97d5804-295d-4a63-b66d-23cafcd44586/ssr-40da)
- [Battery charging circuit of DS3231 module — One Transistor](https://www.onetransistor.eu/2019/07/zs042-ds3231-battery-charging-circuit.html)
- [LEDC with OUTPUT_OPEN_DRAIN — ESP32 Forum](https://esp32.com/viewtopic.php?t=15783)
- [LED Control (LEDC) — Arduino-ESP32](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/ledc.html)
