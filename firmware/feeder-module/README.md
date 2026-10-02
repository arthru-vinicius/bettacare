# Firmware do módulo alimentador (ESP32-C3) — 1.0.0-beta

O módulo que dosa a ração do betta um grão por vez, conferindo cada grão num
feixe infravermelho. Funciona sozinho — agenda, relógio, botão e tela
próprios — e, com o cabo de 4 vias, conversa com o ESP32 principal, que leva
tudo ao servidor e ao app.

Montagem e pinos em [`docs/pinagem-alimentador-modulo.md`](../../docs/pinagem-alimentador-modulo.md);
a estrutura impressa (base, torre e topo) em
[`docs/modelagem-alimentador/`](../../docs/modelagem-alimentador/). O lado do
principal do enlace é `firmware/bettacare/feeder_link.h`.

> **Beta:** a lógica está coberta por testes de host, e o firmware compila sem
> nenhum aviso. Nada disso substitui a bancada: o mecanismo ainda não existe
> fisicamente, e os tempos do doseador (`params.h`) são o ponto de partida, não
> o valor final. Siga a [primeira montagem](#primeira-montagem-na-bancada)
> antes de pôr o módulo no aquário.

---

## O que garante

| Garantia | Como |
|---|---|
| **No máximo 3 refeições em 24 h**, somando agenda, botão e app | Contadas no histórico gravado (pela hora) e, desde o boot, pelo `millis()`; vale a maior. O botão nunca passa do limite; o app passa só com o "alimentar mesmo assim" (`FEED ... FORCE`) |
| Refeição que começou **conta**, mesmo que a energia caia no meio | O histórico é gravado antes do primeiro movimento. Na dúvida, menos ração |
| Agenda **não roda sem hora válida** | DS3231 lido e conferido (BCD, bit de parada do oscilador); sem ele e sem NTP, nada de alimentar na hora errada |
| Refeição perdida com o módulo desligado é **recuperada**, sem dobrar | Até 4 h de atraso, e só se nenhuma outra refeição estiver a menos de 4 h — nem a próxima da agenda, nem a última que saiu |
| **Nenhum motor fica ligado** por engano | Teto absoluto por motor (M1 3 s, M2 6 s, refeição inteira 90 s), à parte da máquina de estados; *watchdog* de 5 s no loop |
| Servo **sem tranco** | 5° a cada 10 ms em todo movimento, inclusive na calibração; PWM só durante o movimento, depois o servo fica solto |
| Sensor quebrado **não deixa o peixe sem comer** | Autoteste antes de cada refeição (LED IR apagado: "bloqueado"; aceso: "livre"). Reprovado, a refeição sai contada pelo servo, um movimento por grão, e termina como `SENSOR` |
| Reservatório vazio ou slide travado **param a dosagem** | Três movimentos seguidos sem grão: para, como `VAZIO` |
| Ração **não empelota** parada | M1 vibra pelo menos uma vez por dia, com refeição ou sem |
| Atualização por OTA **não corta uma refeição ao meio** | Durante o OTA nada começa; o que estiver em andamento para em estado seguro |
| Log do sistema **não corrompe o enlace** | O enlace usa a UART1 nos pinos da UART0, que é o console do ESP-IDF |
| Sensor **desconectado não passa** no autoteste | Pull-up interno no `GPIO3`: fio solto lê sempre "bloqueado", nunca um nível ao acaso |

---

## Arquivos

A lógica que decide se o peixe come não toca em hardware, e é ela que os
testes de host cobrem.

| Arquivo | O que é | Testado no PC |
|---|---|---|
| `params.h` | Todo número de comportamento: limites, tempos, ângulos de fábrica | — |
| `types.h` | Agenda, calibração, histórico, resultado | — |
| `schedule.*` | Agenda, recuperação, refeição perdida, limite de 24 h | `test_schedule` |
| `doser.*` | O ciclo físico de uma refeição, como máquina de estados | `test_doser` |
| `protocol.*` | O texto do enlace com o principal | `test_protocol` |
| `gestures.*` | Toques e toque longo a partir de pressões e solturas | `test_gestures` |
| `timeutil.*` | Data e hora sem biblioteca | `test_schedule` |
| `hw.*` | A fronteira com o hardware: PWM, servo, feixe IR, LED | — |
| `button.*` | Amostragem do botão por timer (2 ms) | — |
| `clock.*` | DS3231 lido nos registradores, NTP pelo Wi-Fi | — |
| `store.*` | NVS: agenda, calibração e histórico, um bloco por chave | — |
| `link.*` | O UART com o principal e a fila de avisos | — |
| `app.*` | Quem decide: ocupado, calibração, limite; a agenda e o M1 diário | — |
| `ui.*` | Botão, tela e LED da placa | — |
| `console.*` | Console da bancada, pela USB ou pela rede | — |
| `debuglog.*` | O log: USB, RAM que sobrevive a reinício (`/log`) e UDP | — |
| `ota_manager.*`, `wifi_manager.*` | Wi-Fi, OTA, a página de estado, `/log` e `/console` | — |
| `tools/udplog.mjs` | Receptor do log no PC, para rodar no Docker | — |

---

## Gravar

Placa `esp32:esp32:esp32c3:CDCOnBoot=cdc` (core 3.3.8). Bibliotecas: U8g2,
ESPAsyncWebServer, AsyncTCP, ElegantOTA.

1. `cp config.example.h config.h` nesta pasta e preencha Wi-Fi e a senha de OTA
   (própria deste módulo). O `config.h` fica fora do git.
2. Compile:
   ```sh
   arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc --warnings all firmware/feeder-module
   ```
3. Grave por OTA em `http://<IP do módulo>/update` (o bootstrap que já está na
   placa tem o ElegantOTA), ou pelo USB-C. O USB desta placa tem mau contato:
   ele fica para recuperação.

O app ocupa 87% da partição (1,15 MB de 1,25 MB). Há folga para correções; se
um dia apertar, o esquema "Minimal SPIFFS" dá 1,9 MB por app, com OTA.

O console da bancada é a própria USB, a 115200 baud — ou a rede, sem cabo
nenhum (próxima seção). `ajuda` lista os comandos.

---

## Depuração pela rede

A placa na bancada raramente está no USB do PC. Tudo o que o console faz pela
USB, ele faz pela rede, com o login do OTA:

| Endereço | O que é |
|---|---|
| `GET http://<IP>/` | Estado (sem login), com o motivo do último reinício |
| `GET http://<IP>/log?desde=N` | As últimas ~3 KB do log, linhas numeradas (`número segundos texto`), a partir da `N+1`. O cabeçalho `X-Log-Proxima` dá o número da próxima |
| `POST http://<IP>/console` com `cmd=...` | Roda um comando do console na próxima volta do loop; a saída vai para o log |

O log fica numa parte da RAM que sobrevive a reinício: depois de um
*watchdog*, de uma exceção, de uma queda de tensão (o servo puxando demais) ou
de um OTA, o `/log` ainda mostra o que veio antes, separado por uma linha
`----- reinicio (motivo) -----`. Só a falta de energia apaga.

```sh
curl -u admin:<senha do OTA> --data-urlencode "cmd=estado" http://<IP>/console
curl -u admin:<senha do OTA> http://<IP>/log
```

**Acompanhando no PC, com Docker** (`tools/udplog.mjs`, na imagem `node:24-alpine`):

```sh
# Puxando o /log a cada 2 s — funciona com qualquer firewall. O login sai do
# config.h montado, nunca da linha de comando.
docker run -d --name bettacare-feeder-log -e MODO=http -e MODULO=http://<IP do módulo> \
  -v "$PWD/firmware/feeder-module/tools:/tools:ro" \
  -v "$PWD/firmware/feeder-module/config.h:/config.h:ro" \
  node:24-alpine node /tools/udplog.mjs
docker logs -f bettacare-feeder-log
```

Também dá para o módulo **empurrar** cada linha por UDP, com `DEBUG_LOG_HOST`
(o IP do PC) no `config.h` e o receptor em `MODO=udp` (`-p 5514:5514/udp`).
No PC da bancada (2026-10-02) o firewall do Windows barra esse UDP — a regra
do Docker Desktop só vale para redes "Públicas", e a rede é "Privada" —, por
isso o modo HTTP é o padrão.

No primeiro boot de todos (NVS vazia), o M1 dá o pulso diário em ~1 min: é o
"nunca pulsou" do anti-empacamento, e não se repete por 24 h.

---

## Primeira montagem, na bancada

Com um copo sob o bico, o módulo fora do aquário e o cabo do principal
desconectado.

1. **Antes de encaixar o braço do servo:** `servo 40`. O eixo vai para os 40°
   do repouso de fábrica e o servo solta. Encaixe o braço com o slide em
   repouso — bolso inteiro sob o reservatório.
2. **Sensor:** `teste sensor` precisa dar "Autoteste ok". Depois, `teste feixe
   20` e passe grãos pelo tubo, à mão: cada um tem de aparecer uma vez só. Grão
   que não aparece, ou que conta dois, é o trimpot do LM393 ou o alinhamento
   do par IR (`docs/pinagem-alimentador-modulo.md`, §4).
3. **Calibração:** `calibrar`, e ajuste pelo botão — 1× +2°, 3× −2°, 2× alterna
   entre repouso e despejo, segurar 2 s grava. O slide anda devagar até cada
   ângulo. Repouso: bolso inteiro sob o reservatório. Despejo: bolso inteiro
   sobre o tubo. Quem já tem os números: `calibrar 38 96`.
4. **Uma dose:** `teste grao`, várias vezes. Cada uma faz o ciclo completo
   (aviso, autoteste, M1, slide) com 1 grão e diz se o grão foi visto. Não conta
   como refeição. Dez em dez antes de seguir.
5. **Motores:** `teste m1` (2 s) e `teste m2` (o aviso: rampa, firme, rampa).
6. **Hora:** com Wi-Fi, o NTP acerta sozinho em segundos. Sem Wi-Fi:
   `hora 2026-10-02 18:30`.
7. `estado` — confira tudo de uma vez.
8. Só então ligue o cabo no principal. No `/status` de lá, `feeder.present`
   vira verdadeiro em ~1 s, e `connected` no primeiro `PONG`.

Sem calibração gravada, nenhuma refeição sai — nem a da agenda: o módulo
recusa com `CALIBRAR`, e o app mostra o aviso.

---

## Botão e tela

A tela fica apagada; acende 5 s a cada toque, durante a refeição e na
programação. O LED da placa acende durante a refeição e **pisca** quando há
algo para ver (refeição incompleta, sensor reprovado, refeição da agenda que
não saiu) — até um toque qualquer no botão.

| Gesto | Normal | Programação | Calibração |
|---|---|---|---|
| 1× | acende; acesa, passa a página | +1 | +2° |
| 2× | **alimenta agora** (dentro do limite) | próximo campo | alterna repouso/despejo |
| 3× | testa o aviso ao peixe, sem ração | −1 | −2° |
| segurar 2 s | entra na programação | grava e sai | grava e sai |
| parado | apaga em 5 s | cancela em 20 s | cancela em 2 min |

Páginas: **hora** (com a próxima refeição, a última e as refeições em 24 h no
canto); **últimas 24 h** (refeições, última dose, sensor, calibração);
**conexões** (principal, Wi-Fi, relógio, versão).

A programação edita os dois horários (hora cheia), os grãos (1 a 20) e o
automático, na RAM. Só o "segurar" grava; a agenda nova vale da próxima
ocorrência em diante.

---

## Console

| Comando | Faz |
|---|---|
| `estado` | Tudo: hora, Wi-Fi, principal, agenda, calibração, refeições em 24 h, última refeição, sensor, doseador |
| `hora` / `hora AAAA-MM-DD HH:MM[:SS]` | Mostra / acerta o relógio (hora do aquário) |
| `agenda` / `agenda H1 H2 GRAOS AUTO` | Mostra / grava, ex.: `agenda 5 17 5 1` |
| `alimentar [GRAOS]` | Como o botão: conta no limite |
| `alimentar [GRAOS] forcar` | Passa do limite, como o "alimentar mesmo assim" do app |
| `calibrar` / `calibrar REPOUSO DESPEJO` | Calibração pelo botão / gravada direto |
| `servo GRAUS` | Leva o slide até lá, devagar, e solta o servo |
| `teste m1` / `teste m2` | Pulso do M1 / aviso ao peixe |
| `teste sensor` / `teste feixe [SEGUNDOS]` | Autoteste do par IR / autoteste e contagem do que passar |
| `teste grao` | Uma dose de 1 grão, fora do limite e do histórico (despeja de verdade) |
| `reiniciar`, `fabrica sim` | Reinicia / apaga agenda, calibração e histórico |

Os testes não são refeição: não entram no limite, no histórico nem no
principal. Por isso o `teste grao` é com o copo — no aquário, ele alimenta.

---

## Regras de alimentação

Decididas pelo Arthur em 2026-10-02; os números estão em `params.h`.

- **Agenda:** dois horários em hora cheia (fábrica: 05 h e 17 h, 5 grãos). A
  refeição sai na hora e até 1 h depois dela (o módulo pode estar ocupado ou
  sem hora por um instante).
- **Limite:** 3 refeições em 24 h, somando agenda, botão e app. A agenda e o
  botão param nele; o módulo avisa (`DENIED LIMITE`) e o app oferece "alimentar
  mesmo assim".
- **Recuperação:** uma refeição da agenda perdida com o módulo desligado é
  servida ao ligar, se o atraso for de até 4 h e nenhuma outra refeição estiver
  a menos de 4 h — a próxima da agenda ou a última que saiu (do botão, do app,
  ou outra recuperada). Com uma refeição da agenda na hora agora, ela tem a vez
  e a outra fica perdida. Perdida, sai `DENIED PERDIDA` e o LED pisca.
- **Agenda mudada:** vale da próxima ocorrência, só no que mudou. Mudar os grãos
  na hora de uma refeição não a cancela; trocar um horário para "agora" não
  alimenta na hora; religar o automático depois de dias não gera refeições
  perdidas.
- **Sensor reprovado no autoteste:** a refeição sai contada pelo servo (um
  movimento por grão, sem repetir) e termina como `SENSOR`; o app avisa.
- **M1:** em toda refeição, e sozinho se passar 24 h sem pulso.

---

## Enlace com o principal

Uma linha ASCII por mensagem, 9600 baud, `GPIO20` (RX) e `GPIO21` (TX). O
principal pergunta (`PING`) a cada 2 s; avisos que saem com ele fora do fio
esperam numa fila curta (4) e vão na reconexão, depois do `SCHEDULE`.

```text
principal → módulo   PING
                     FEED [grãos] [FORCE]
                     CFG <h1> <h2> <grãos> <0|1>

módulo → principal   PONG <h1> <h2> <grãos> <auto> <idade_s> <req> <conf> <ok> <refeições_24h>
                     SCHEDULE <h1> <h2> <grãos> <auto>
                     FED <req> <conf> <ok> <OK|SENSOR|VAZIO> <AGENDA|RECUP|BOTAO|APP|FORCADO>
                     DENIED <LIMITE|OCUPADO|CALIBRAR|PERDIDA> <refeições_24h> <origem>
```

Linha com qualquer campo fora da faixa é descartada inteira, dos dois lados:
o enlace não tem checksum, e um dígito trocado por ruído não pode virar
"alimenta às 47 h".

---

## Testes de host

```sh
sh firmware/feeder-module/test/run.sh
```

Precisa de um `g++` com C++17 no `PATH` (nesta máquina, o MinGW em
`C:\mingw64\bin`). Cada teste inclui o `.cpp` real.

| Teste | Cenários |
|---|---|
| `test_schedule` (38) | Ocorrência certa de cada horário; primeiro boot sem refeições "perdidas"; na hora uma vez só; recuperação até 4 h; perdida com 5 h; recuperação perto da próxima, perto da última (botão) e duas seguidas; a da hora com a vez; horários iguais; automático desligado e religado; agenda mudada: só os grãos, um horário, horários que se separam; refeições em 24 h com o relógio acertado para trás |
| `test_doser` (48) | Refeição de 5 grãos; reservatório vazio e acabando no meio; sensor quebrado contando pelo servo; só o aviso; pulso do M1 com teto; abortar no meio e retomar sem tranco; calibração devagar e estacionando; movimento manual recusado no meio de uma refeição; teste do feixe, com sensor quebrado e com teto |
| `test_protocol` (28) | `PING`, `FEED` nas quatro formas, `CFG`, lixo e campos fora da faixa; o que sai |
| `test_gestures` (12) | 1, 2 e 3 toques; quatro contam como três; toques espaçados; o longo com o dedo ainda no botão |

## Verificado na placa (2026-10-02)

Gravado por OTA na Super Mini da bancada (`192.168.100.47`), ainda sem nada
ligado: boot, Wi-Fi e hora pelo NTP; DS3231 e tela ausentes reconhecidos sem
travar; agenda de fábrica, "não calibrado" e 0 de 3 em `estado`; `teste sensor`
reprovando com o sensor desconectado; o pulso do M1 do primeiro boot; console
pela rede; o log atravessando um `reiniciar` com a numeração seguindo; o
receptor no Docker acompanhando, inclusive a queda e a volta.

## Não verificado — exige o hardware montado

- os tempos do doseador (`SERVO_SETTLE_MS`, `DETECT_WINDOW_MS`) contra o grão
  e o tubo de verdade;
- a sensibilidade do feixe a um grão de 1 mm, e o tubo seco o bastante;
- o M2 na água: intensidade e o que o peixe acha dele;
- a tela e o botão reais, o ruído dos motores no I²C e no botão;
- o enlace com o principal ao vivo (o lado de lá só foi exercitado por teste
  de host).
