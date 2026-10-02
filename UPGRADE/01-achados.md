# Relatório de achados

Síntese da auditoria completa de 2026-08-21: firmware (~3.100 linhas), servidor e
banco (~2.700), contrato (~1.000) e PWA (~2.000), mais a análise elétrica da
montagem. Os detalhes de cada camada estão nos documentos 02 a 06; aqui fica o
que atravessa mais de uma.

---

## O essencial em cinco frases

1. **O sistema é bem construído.** Não há nada aqui que peça reescrita, e vários
   trechos são melhores do que a média do que se vê em projetos desse porte.
2. **Nada disto jamais rodou num ESP32 físico** — a validação de ponta a ponta
   foi feita simulando o dispositivo com `curl`.
3. **Três das quatro interfaces físicas estão fora da especificação do
   componente que controlam**, e falham exatamente como você descreveu:
   intermitente, sem log.
4. **Existe um caminho de falha que liga um resistor de R$ 0,10 ausente a uma
   pane total de telemetria** — e ele apaga o próprio rastro.
5. **O firmware não reporta motivo de reset nem memória livre**, que é o que
   distinguiria um defeito de hardware de um bug de software. Sem isso, todo o
   resto é palpite.

---

## O achado que importa mais

Está detalhado em [C1](05-plano-contrato-e-comunicacao.md#c1--um-único-valor-fora-de-faixa-derruba-a-telemetria-inteira),
e merece estar na primeira página porque atravessa as três camadas e produz um
diagnóstico invertido.

```
falta um resistor de pull-up no tacômetro          (documento 02, §4)
        ↓
a linha capta ruído do PWM de 25 kHz ao lado
        ↓
o firmware conta pulsos falsos → rpm acima de 20.000
        ↓
o Zod rejeita o corpo INTEIRO → HTTP 400            (contrato)
        ↓
o firmware trata 400 como erro transitório: recua para 30 s
e reenvia o mesmo corpo inválido, para sempre       (firmware)
        ↓
o watchdog do servidor: device.offline              (servidor)
        ↓
todos os componentes viram "unknown"
        ↓
o app diz "controlador sem contato"
```

O aquário está funcionando. A luz acende no horário, a ventoinha segue a
temperatura, o botão responde. E o sistema inteiro de observabilidade afirma que
o dispositivo sumiu — porque ele tentou contar o que estava acontecendo.

Pior: `bettacare.ino:104` trunca o RPM para `uint16_t`, então 70.000 vira 4.464.
Às vezes o valor truncado passa e o POST funciona. **Intermitente, sem log, e o
log é justamente o que não consegue chegar.**

A lição estrutural, que vale além deste caso: **um sistema de observabilidade
precisa ser mais robusto que a coisa que ele observa.** Rejeitar o relatório
inteiro porque um componente com defeito relatou um número estranho é
exatamente o contrário do que ele existe para fazer.

---

## As quatro linhas de investigação, e onde cada uma terminou

### "Componentes paravam de funcionar com frequência"

**Causa mais provável: elétrica.** Três interfaces fora de especificação
([documento 02](02-hardware-e-ligacoes.md)):

- **Luminária** — o resistor de 220 Ω em série derruba a tensão de controle para
  ~1,65 V, abaixo dos 3 V mínimos do SSR-40DA. O circuito opera no joelho da
  curva do LED interno, e o joelho anda com a temperatura. O SSR já limita a
  própria corrente; o resistor não protege nada e só tira margem.
- **Ventoinha** — saída push-pull do ESP32 contra um pull-up interno da
  ventoinha, onde a norma Intel exige dreno aberto. Corrente entrando pelo pino
  via diodo de ESD, continuamente. É assim que um GPIO "para de funcionar do
  nada" depois de meses.
- **Tacômetro** — só o pull-up interno de ~45 kΩ, bordas lentas, ruído. Raiz do
  caminho de falha acima.

Some-se a ausência de capacitor de reservatório no trilho de 3,3 V, que é o
suspeito nº 1 de *"às vezes ele simplesmente não funcionava"*: cada rajada de
transmissão Wi-Fi puxa o trilho, e um reset por brownout volta em segundos sem
deixar rastro nenhum.

### "Não haviam logs"

**Resolvido no desenho, incompleto no código.** O `event_log` estruturado existe
e é bom. Mas:

- **Eventos e `ack` são drenados antes do POST** e perdidos se ele falhar
  ([F1](03-plano-firmware.md)) — o firmware faz o contrário do que a
  documentação do próprio módulo promete.
- **`temp.sensor_lost` nunca é emitido** no caminho de falha por CRC
  ([F4](03-plano-firmware.md)). A pergunta *"desde quando o sensor sumiu?"* —
  uma das três que guiaram todo o desenho — não tem resposta nesse caminho.
- **22 dos códigos que o firmware emite não estão no catálogo**
  ([C3](05-plano-contrato-e-comunicacao.md)) e aparecem crus na aba Logs,
  incluindo `fan.pwm_failed`, que é fatal.
- **A dedup só compara com a última entrada** ([F9](03-plano-firmware.md)): um
  componente oscilando alterna códigos, quebra a dedup e enche o buffer de 24
  posições em segundos — descartando justamente o começo do episódio.

### "Às vezes ele simplesmente não funcionava"

Três causas candidatas, e a instrumentação nova distingue as três:

- **Brownout** por falta de capacitância — `esp_reset_reason()` prova ou descarta.
- **O botão físico é amostrado a 5 Hz** ([F5](03-plano-firmware.md)). Um toque de
  80–150 ms cabe inteiro entre duas amostras e **não é visto**. Do ponto de vista
  do firmware, não houve toque — nada no log, irreproduzível.
- **O caminho C1 acima**, que faz o dispositivo parecer offline estando são.

### "Quero um banco com informação rica de saúde"

**O banco guarda o estado do aquário, não o estado do controlador.** Nenhuma
destas perguntas tem resposta hoje: o ESP32 reiniciou esta noite e por quê; a
memória está caindo ao longo dos dias; o POST está mais lento; o potenciômetro
tem mau contato; quantas vezes seguidas o dispositivo falhou em falar.

A causa é que os dados não chegam ([F7](03-plano-firmware.md)), e a correção
precisa das três pontas na ordem certa. DDL concreta em
[S1](04-plano-servidor-e-banco.md).

---

## Promessas do desenho sem implementação

`docs/arquitetura-observabilidade.md` é um documento bom, e é por isso que vale
listar onde o código não o alcançou. Nenhum destes é bug — são lacunas.

| Prometido | Estado real | Onde |
|---|---|---|
| Saúde de `button` e `pot` | Nunca avaliados. `pot` continua invisível — que era o exemplo citado no próprio documento | [S2](04-plano-servidor-e-banco.md) |
| Saúde de `api` por falhas consecutivas | Hardcoded `ok`. O dado existe no firmware e nunca é enviado | [S3](04-plano-servidor-e-banco.md) |
| `cmd.device_offline` para comando não entregue | Não existe. Comandos em `queued` nunca expiram | [S4](04-plano-servidor-e-banco.md) |
| Luminária em `fault` por divergência | Estruturalmente impossível: o firmware reporta o que comandou | [F10](03-plano-firmware.md) |
| Dedup impedindo inundação | Só funciona para repetição imediata | [F9](03-plano-firmware.md) |
| `.h` do firmware gerado a partir do Zod | Não existe. O firmware reimplementa o contrato à mão | [C2](05-plano-contrato-e-comunicacao.md) |
| Busca por texto nos logs | Servidor aceita `q`; o front nunca envia | [P2](06-plano-pwa.md) |
| "Servidor sem banco de dados" na interface | Não distinguido de falha de rede | [P3](06-plano-pwa.md) |

Três linhas dessa tabela merecem nota. **A luminária em `fault`** não é
implementável com o hardware atual — o SSR não tem realimentação, e o honesto é
corrigir a promessa no documento em vez de fingir que o diagnóstico existe. **A
saúde de `pot`** é a mais irônica: o documento cita o potenciômetro com mau
contato como exemplo do que era invisível, e ele segue invisível. **O gerador do
`.h`** era a resposta à "classe de bug mais chata do projeto"; sem ele, ela foi
reduzida de três cópias do contrato para duas, não eliminada.

---

## Contagem

| Camada | Achados | Críticos/Altos |
|---|---|---|
| [Hardware](02-hardware-e-ligacoes.md) | 8 interfaces avaliadas | 3 fora de especificação + alimentação |
| [Firmware](03-plano-firmware.md) | 14 | 6 |
| [Servidor e banco](04-plano-servidor-e-banco.md) | 14 | 5 |
| [Contrato](05-plano-contrato-e-comunicacao.md) | 6 | 2 |
| [PWA](06-plano-pwa.md) | 6 | 0 |

---

## O que está certo e não deve ser tocado

Vale tanto quanto a lista de problemas, porque uma próxima passada pode
"simplificar" alguma destas coisas sem entender por que elas estão ali:

- **A separação em dois núcleos do firmware.** Era o defeito estrutural do
  sistema antigo — rede bloqueando o botão e a ventoinha — e está resolvido de
  verdade.
- **O cache de minutos do RTC** (`rtc_manager.cpp:38-49`), que evita um deadlock
  real entre o mutex do log e o do I²C. A melhor decisão do firmware.
- **`ingest/auth.ts` inteiro** — SHA-256 dos dois lados antes do
  `timingSafeEqual`, resolvendo o vazamento de comprimento. O melhor arquivo do
  repositório.
- **A idempotência por `last_seq` em `device_state`**, e o raciocínio que a
  levou para lá em vez de uma constraint única impossível numa tabela
  particionada.
- **Particionamento mensal com purga por `DROP`**, porque `DELETE` não devolve
  espaço e `VACUUM FULL` é inaceitável num Postgres compartilhado.
- **O rollup ponderado por duração**, com o `coalesce` dentro do `least` — a
  armadilha que faria toda hora sem luz virar 60 minutos acesa.
- **A escolha dos pinos do ESP32.** Nenhum strapping, nada na faixa da flash,
  ADC1 para o potenciômetro. Quem montou sabia o que estava fazendo; os
  problemas estão nos componentes ao redor, não na atribuição.
- **O service worker e o ciclo de vida do comando na interface**, que
  implementam o desenho fielmente.
- **Todo inteiro do contrato declarando um teto igual ao da coluna que o
  recebe** (`primitives.ts:88-100`). A regra está certa; o C1 mostra que falta
  decidir o que fazer quando ela é violada.

---

## A ordem que decorre de tudo isso

Detalhada no [roteiro de execução](07-roteiro-de-execucao.md). Em uma frase:

> **Hardware e instrumentação primeiro, em paralelo; depois contrato, servidor e
> firmware nessa ordem; o PWA por último — exceto os cinco ajustes dele que não
> dependem de nada.**

E a regra que não pode ser invertida: **contrato → servidor → firmware.** Fazer
o firmware primeiro **não dá erro** — o Zod remove campos desconhecidos em
silêncio ([C5](05-plano-contrato-e-comunicacao.md)) — e é exatamente por isso
que é perigoso.
