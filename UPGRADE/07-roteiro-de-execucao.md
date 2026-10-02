# Roteiro de execução

Ordem, dependências e critérios de aceite da rodada. Os planos por camada estão
nos documentos 02 a 06; este aqui é a sequência.

**Nada aqui foi implementado.** Cada fase é um pacote de trabalho pensado para
ser entregue a um agente especializado, com contexto suficiente para ele
trabalhar sem reabrir as decisões.

---

## O princípio que ordena tudo

Duas regras, e as duas vêm de achados concretos:

**1. Contrato → servidor → firmware. Nunca o contrário.**
`telemetryRequestSchema` usa `z.object()`, que **remove** chaves desconhecidas em
vez de rejeitá-las ([C5](05-plano-contrato-e-comunicacao.md)). Um firmware que
comece a mandar o bloco `diag` antes de o servidor conhecê-lo recebe `200`, e o
dado é descartado em silêncio. Tudo parece funcionar. Inverter a ordem não dá
erro — por isso é perigoso.

**2. Instrumentação antes de conserto.**
Metade dos achados de hardware são hipóteses fundamentadas, não fatos medidos. O
campo `reset_reason` custa nada e decide sozinho se o problema é elétrico ou de
software. Consertar antes de medir é trocar peça no escuro.

---

## Fase 0 — Medir, antes de mexer em qualquer coisa

**Depende de:** nada. **Bloqueia:** as fases 1 e 4.

Três medições de multímetro, listadas em
[02 §10](02-hardware-e-ligacoes.md#10--roteiro-de-bancada). Com o ESP32 no USB,
fora do aquário.

| # | Medição | Decide |
|---|---|---|
| 1 | Tensão `SSR +` → `SSR -` com a luz comandada ligada | Se < 3 V, o defeito da luminária está confirmado |
| 2 | Tensão pino 4 → pino 1 da ventoinha, **ESP32 desconectado** | 3,3 V → correção só em firmware; 5 V → precisa de MOSFET **e o duty inverte** |
| 3 | Resistência `DATA` → `3V3` do DS18B20, sem energia | Circuito aberto = falta o pull-up do 1-Wire |

A medição 2 é a que mais muda o trabalho adiante: ela decide se `fan.cpp` precisa
passar a escrever o complemento do duty. **Ninguém deve encostar em `fan.cpp`
antes dessa leitura.**

Vale aproveitar a bancada para confirmar o tipo da ventoinha (norma Intel §3.4):
com duty em 0 %, ela para ou continua no mínimo? A resposta decide se a regra
`fan.tach_stalled` tem falso positivo estrutural
([S12](04-plano-servidor-e-banco.md), [F23](03-plano-firmware.md)).

**Critério de aceite:** as três leituras anotadas, e o tipo da ventoinha
identificado.

---

## Fase 1 — Hardware

**Depende de:** Fase 0. **Paralela a:** Fases 2 e 3.

Correções elétricas, na ordem de retorno. Detalhes e diagramas em
[02](02-hardware-e-ligacoes.md).

1. **Capacitor de 470 µF + 100 nF no trilho de 3,3 V.** Maior retorno por real de
   toda a rodada. Faça mesmo que a medição 1 não confirme nada.
2. **Remover o resistor de 220 Ω** do GPIO23. Se for fazer a versão robusta,
   MOSFET 2N7002 no lado baixo com o SSR em 5 V.
3. **Pull-up de 4,7 kΩ no DS18B20** (2,2 kΩ se o cabo for longo).
4. **Pull-up de 4,7–10 kΩ no tacômetro**, mais o filtro RC de 1 kΩ + 10 nF.
5. **Fonte única**, se quiser fechar o assunto: 12 V → buck → 5 V no `VIN`.

Lista de compras completa em [02 §9](02-hardware-e-ligacoes.md#9--lista-de-compras).

**Critério de aceite:** com um osciloscópio no trilho de 3,3 V, as quedas
durante transmissão Wi-Fi ficam abaixo de 200 mV. A luminária liga e desliga
cem vezes seguidas sem falhar.

**Atenção:** o aviso de segurança AC de `docs/pinagem-e-montagem-esp32.md`
continua integralmente válido. O lado do SSR que comuta a rede não é trabalho
para fazer com pressa.

---

## Fase 2 — Contrato

**Depende de:** nada. **Bloqueia:** Fases 3, 4 e 6.

É a fase mais curta e a que mais destrava.

1. **Tolerância a dado ruim** ([C1](05-plano-contrato-e-comunicacao.md)) —
   validação por bloco no ingest, com bloco inválido virando `null` mais um
   evento `ingest.field_rejected`. Núcleo obrigatório: `device_id`, `boot_id`,
   `seq`.
2. **`diagSchema` opcional**, com todos os campos de
   [F7](03-plano-firmware.md) e tetos casando com as colunas da migration.
3. **Contador de chaves desconhecidas** no ingest, registrado uma vez por versão
   de firmware vista.
4. **Os 22 códigos que faltam** no catálogo ([C3](05-plano-contrato-e-comunicacao.md)),
   e resolver `api.server_error` vs `api.server_unreachable`.
5. **`button` e `pot` em `AQUARIUM_COMPONENTS`** — uma linha, e é ela que faz as
   duas aparecerem na aba Saúde depois.

**Critério de aceite:** `curl` com `"rpm": 99999` recebe `200`, o resto do estado
é gravado, e a aba Logs mostra o campo rejeitado com o valor recebido.

---

## Fase 3 — Servidor e banco

**Depende de:** Fase 2. **Bloqueia:** Fase 4.

1. **Migration `0004`** com as colunas de diagnóstico
   ([S1](04-plano-servidor-e-banco.md) e [S9](04-plano-servidor-e-banco.md)).
   **Só aditiva** — preserva a garantia de rollback seguro de
   `docs/deploy-cli-homelab.md`.
2. **Gravar os campos novos** em `upsertState` e na linha de `telemetry`.
3. **Regras de saúde que faltam**: `evaluateButton`, `evaluatePot`, e a regra
   real de `api` a partir de `api_failures` e `post_latency_ms`.
4. **Ciclo de vida do comando**: TTL para `queued`, evento `cmd.device_offline`,
   teto de idade para entrega, e `cmd.late_ack`.
5. **Rollup por cobertura**, não por janela fixa.
6. **Tirar o `"aquarium-01"` hardcoded** de `jobs/maintenance.ts`.
7. **Teto de 16 KB** no corpo do ingest, verificado antes de ler.

O piso de duty da regra de `fan` ([S12](04-plano-servidor-e-banco.md)) depende do
tipo de ventoinha identificado na Fase 0.

**Critérios de aceite:**
- Um POST com `diag` completo é aceito, e um sem ele também.
- Com o dispositivo desligado, um comando enfileirado gera `cmd.device_offline`.
- Derrubar o servidor por dois dias e voltar: os relatórios das horas perdidas
  aparecem na manutenção seguinte.

---

## Fase 4 — Firmware

**Depende de:** Fases 0, 2 e 3. **A de maior risco.**

É a fase em que um erro exige recuperação física do dispositivo. O aviso de
`docs/firmware.md` continua valendo palavra por palavra: **testar em bancada, com
o ESP32 no USB, antes de gravar no que está no aquário. Se o firmware novo
quebrar a rede, o OTA não salva.**

**4a. Instrumentação** ([F7](03-plano-firmware.md), [F13](03-plano-firmware.md))
— ampliar `DeviceSnapshot`, preencher, serializar no bloco `diag`. RSSI nulo em
vez de `-120`. **Priorize `reset_reason` acima de tudo.**

**4b. Saneamento de dado ruim** ([C1](05-plano-contrato-e-comunicacao.md)) —
limitar `rpm` na origem com `fan.tach_implausible`, e tratar `400` como
determinístico: registrar, avançar o `seq`, seguir.

**4c. Os quatro bugs de comportamento** — [F1](03-plano-firmware.md) (drenar
`ack` só após `200`), [F2](03-plano-firmware.md) (reboot confirmado pela rede),
[F4](03-plano-firmware.md) (`temp.sensor_lost` no ponto de decisão),
[F5](03-plano-firmware.md) (interrupção no botão — superada pela amostragem,
ver [Rodada de bancada](#rodada-de-bancada--2026-10-01)).

**4d. Robustez de longo prazo** — [F3](03-plano-firmware.md) (ressincronizar
NTP), [F6](03-plano-firmware.md) (mutex no `wifi_manager`),
[F11](03-plano-firmware.md) (task watchdog).

**4e. Correção do PWM**, se a Fase 0 apontar 5 V — dreno aberto ou MOSFET, com a
inversão do duty em `_apply_speed()`. **Confirmar no osciloscópio.**

**4f. Qualidade e higiene** — [F9](03-plano-firmware.md), [F10](03-plano-firmware.md),
[F12](03-plano-firmware.md), [F8](03-plano-firmware.md), [F14](03-plano-firmware.md).
O F8 (eliminar `String` do laço quente) **só depois** de ter `max_alloc_heap` em
mãos: sem confirmação de fragmentação, é otimização sem causa.

**Critérios de aceite:**
- Toques rápidos no botão acendem a luz **sempre**.
- Com o servidor derrubado no meio de um comando, a interface mostra o desfecho
  correto quando ele volta.
- O cenário "servidor fora do ar por dois dias" roda até o fim sem intervenção, e
  o relógio continua certo depois.
- O flash continua abaixo de 90 % (85 % no plano; 86 % depois da rodada de
  bancada de 2026-10-01).

---

## Fase 5 — PWA, parte independente

**Depende de:** nada. **Pode ser feita a qualquer momento**, inclusive primeiro,
se quiser um ganho visível cedo.

Os cinco ajustes de [06](06-plano-pwa.md) que não dependem de dado novo: P1
(polling rápido), P2 (busca nos logs), P3 (`db_unavailable`), P5 (`aria-live`),
P6 (recuo).

**Critério de aceite:** derrubar o Postgres com a interface aberta; a mensagem
fala em banco de dados, não em rede.

---

## Fase 6 — PWA, parte dependente

**Depende de:** Fases 2, 3 e 4.

Salvaguarda do "enviando" indefinido, cartão "Controlador" no hub de
Diagnóstico, série de heap nos Relatórios. As linhas de `button` e `pot` na aba
Saúde aparecem sozinhas — a tela já renderiza a lista que vem da API.

---

## Fase 7 — Fechar a fonte única

**Depende de:** Fase 4 estabilizada. **Opcional, e vale.**

[C2](05-plano-contrato-e-comunicacao.md): o gerador de
`firmware/bettacare/contract_generated.h`, verificado na CI. Comece pelo teste de
consistência de enums, que é barato e já pega a próxima divergência.

---

## Visão geral das dependências

```
Fase 0 ── medir ──┬─────────────► Fase 1  hardware
                  │
                  └──────────────────────────────┐
                                                 │
Fase 2 ── contrato ──► Fase 3 ── servidor ──► Fase 4 ── firmware ──► Fase 7
                              │                      │
                              └──────────────────────┴──► Fase 6  PWA dependente

Fase 5 ── PWA independente ── (sem dependências, a qualquer momento)
```

Caminho crítico: **0 → 2 → 3 → 4**. As fases 1 e 5 correm em paralelo sem
disputar nada.

---

## Atualizações de documentação que a rodada exige

Não são opcionais: são divergências entre o que `docs/` afirma e o que o código
faz, e deixá-las no ar recria a confusão.

| Documento | O que corrigir |
|---|---|
| `docs/arquitetura-observabilidade.md` §3 | A luminária em `fault` por divergência **não detecta defeito de hardware** — o firmware reporta o que comandou. Ela detecta conflito entre fontes de controle, que é outra coisa |
| `docs/arquitetura-observabilidade.md` §2 | A dedup não impede inundação por códigos alternados, só por repetição imediata |
| `docs/pinagem-e-montagem-esp32.md` | O resistor de 220 Ω sai; o pull-up do 1-Wire deixa de ser condicional; entram os pull-ups do tacômetro e o capacitor de reservatório. "Duty 0 % desliga a fan" depende do tipo da ventoinha |
| `docs/firmware.md` | Atualizar a lista "Não verificado" conforme a bancada for cobrindo os itens |
| `docs/api-servidor.md` | O comportamento novo de aceitação degradada e o código `ingest.field_rejected` |
| `docs/plano-migracao-bettacare.md` | A promessa do `.h` gerado — ou cumprir (Fase 7) ou registrar que não foi cumprida |

---

## Como medir se a rodada funcionou

Quatro perguntas. Se as quatro tiverem resposta no app, sem ninguém abrir um
terminal, a rodada cumpriu o objetivo.

1. **O ESP32 reiniciou nas últimas 48 h, e por quê?**
   Aba Diagnóstico → Logs, filtro `system`. O `reset_reason` distingue brownout
   de watchdog de reboot comandado.
2. **Desde quando o sensor de temperatura está fora?**
   Aba Saúde → `temp` → "desde HH:MM", com o histórico de transições.
3. **Por que a luminária não acendeu quando eu apertei no app?**
   O aviso abaixo do botão, com a razão em português vinda de um `code`.
4. **A memória livre está caindo ao longo dos dias?**
   Aba Relatórios → série de heap.

As três primeiras são as perguntas que abrem
`docs/arquitetura-observabilidade.md`. A quarta é a que esta rodada acrescentou.

---

## Rodada de bancada — 2026-10-01

A primeira montagem completa em bancada achou defeitos que nenhum plano acima
previa. Os comentários do firmware que citam "UPGRADE/07" apontam para cá.

**A regra que saiu dela:** antes de culpar solda ou componente, provar ou
descartar o firmware com um experimento medido pelo próprio ESP32. Horas foram
gastas ressoldando e medindo resistência, com um multímetro de bateria fraca
dando falso diagnóstico de terra ruim, enquanto o defeito principal era de
firmware e saiu com um experimento de minutos.

### O relé que trocava sozinho a cada ~5 s

- **Sintoma:** a luminária acendia e apagava sem comando; segurar o botão
  acendia, soltar apagava.
- **Como foi achado:** firmware instrumentado por OTA (hoje
  `firmware/bettacare-debug`), com um anel de eventos e chaves que desligam
  cada subsistema em tempo real. Com o DS18B20 a 5 s: 672 picos no `GPIO18` e 8
  trocas em 20 s. Desligado: zero e zero em 25 s. A 7 s, o "fantasma" mudou de
  ritmo junto.
- **Causa:** os bit-slots do 1-Wire no `GPIO19` induzem picos de microssegundos
  no fio vizinho do botão (16 no pedido de conversão, 152 na leitura). A
  interrupção do F5 aceitava qualquer borda como toque, e o debounce de 50 ms
  era inócuo com o laço a 200 ms.
- **Correção:** amostragem do nível por `esp_timer` a cada 2 ms — toque só com
  30 ms seguidos em baixo, soltura só com 30 ms em alto (`light.cpp`).
- **Resultado:** 32 min, 62.909 picos, 4 trocas — os 4 toques reais.

### O `GPIO27` queimado

O corte de energia da ventoinha (AO3400 no retorno) estava no `GPIO27`, que
parou de obedecer: o pino ficava entre 3,3 e 3,23 V qualquer que fosse o valor
escrito. Causa mais provável: curto gate/dreno no AO3400 durante a montagem,
expondo o pino a 12 V por 1 kΩ. Foi para o `GPIO26`, e o firmware passou a ler
o pino de volta a cada 2 s (`fan.power_pin_fault`, um aviso por episódio).

### Robustez de longo prazo

| Defeito | Efeito | Correção |
|---|---|---|
| `esp_task_wdt_init` falhava com o watchdog que o core já sobe, e o erro era ignorado | O prazo real era 5 s, não 60: um NTP lento reiniciava o chip, e sem internet virava laço de reboot | `esp_task_wdt_reconfigure`; NTP com 1 s por tentativa, alimentando o watchdog |
| Só a task de rede era vigiada | Loop travado congelava luz, ventoinha e botão em silêncio | `enableLoopWDT()` — testado travando o loop de propósito: reinício em ~60 s, `task_wdt` |
| `RTC_DS3231::now()` ignora falha de I²C e decodifica lixo da pilha | "40:08" no cache; lixo com cara de hora válida trocaria a luz | Leitura direta e conferida (transação + BCD 24 h); 3 falhas = módulo ausente |
| Módulo de relógio que parasse em operação seguia "disponível" | Hora congelada, automação parada sem aviso | `rtc.missing` e procura a cada 15 s; enquanto isso, a hora do ESP32 via NTP |
| Uma leitura de hora errada, mas válida, trocava a luz | Luz piscando 10 s e override manual perdido na volta | Virada de período só com confirmação da leitura seguinte |
| 85,0 °C (valor de power-on do DS18B20) aceito | Ventoinha a 100 % e 30 min de cooldown | Descartado como leitura inválida (`temp.reset_value`) |
| HTTP 400 contava como sucesso | `/status` dizia "contato há 1 s" enquanto o painel dizia "sem contato" | 400 não avança `last_success_ms`; `last_http` no `/status` |
| RX do UART2 flutuando com o conector vazio | Ruído marcava o alimentador como conectado | Pull-up interno no `GPIO16`; só linha reconhecida conta |
| Campos do alimentador sem validação | Dígito trocado por ruído viraria "alimenta às 47h" | Faixas do contrato, linha inválida descartada; campo longo não vira dois |
| `fan.power_pin_fault` a cada 2 s | Log inundado durante a falha | Um aviso por episódio, e `fan.power_pin_ok` na volta |
| `delay(2000)` no `setup()` | Luz apagada 2 s a mais a cada reinício | Removido na produção; o firmware de debug mantém |

Tudo validado ao vivo com o firmware de debug, injetando cada falha por comando
(ver `firmware/bettacare-debug/README.md`), e a lógica do relógio e do enlace do
alimentador também por teste de host, com o `.cpp` de produção.

### Da ventoinha, antes disso no mesmo dia

O buffer de MOSFET do PWM (o caso dos 5 V, §7 do guia de pinagem) inverte o
duty, e o firmware nunca tinha implementado a inversão — 0 % chegava como
100 %. Junto vieram: o estado ocioso do automático passou a impor velocidade
zero (antes, saindo do modo de segurança, a ventoinha ficava presa na
velocidade anterior), a zona morta do potenciômetro foi para 110 no ADC e a
pilha da task de rede para 16 KB, depois de um estouro que derrubava o
dispositivo em laço.

---

## Rodada de servidor, banco e PWA — 2026-10-02

Depois da bancada, a revisão do resto do sistema. O agente do homelab
(`homelab-c4`) levantou os números de produção que guiaram as correções: 277
POSTs recusados inteiros e 300 `light.state_mismatch` num só dia.

| Defeito | Correção |
|---|---|
| Saúde contava anomalia por número de POSTs | Por tempo (5 s; botão, 30 s) — necessário para a telemetria a 1 s |
| "Desejado" da luz valia para sempre depois de um comando | Só vale enquanto a última mudança veio do comando |
| Item inválido de `events` derrubava o POST inteiro (o `comp: "feeder"` antes do servidor conhecer o alimentador) | O item sai sozinho, `ingest.event_dropped`; o log mostra o valor recebido |
| `component_status` regravado 11×/POST | Só na mudança, ou a cada 5 s |
| `/healthz` respondia 500 com o banco fora, e o prazo do `select 1` não valia | 503, com o prazo dentro de transação |
| Horários de acender e apagar iguais deixavam a luz acesa para sempre | Recusados pelo contrato |
| Mudança de configuração não deixava rastro | `settings.updated`, com o que mudou e quem mudou |
| Rollup só de madrugada: relatório vazio ou de ontem | A cada hora cheia |
| PWA sem editor de horário nem de limites | Editores ao tocar nos horários e na temperatura |
| Ventoinha sem "ligar"; o botão virava "voltar ao automático" | Ligar/Desligar e "Voltar ao automático" separados |
| Relatórios sem nada para levar embora | Aba Registros com planilhas (CSV para Excel) |

Latência do toque ao "confirmado": de 3–8 s para ~1–2 s, com a telemetria a
1 s (ajuste na Saúde), a confirmação antecipada no firmware e o app
consultando a cada 2 s (0,6 s com comando em voo).

Sem migration e sem variável de ambiente nova — determinações do homelab para
esta rodada. Testes: 41 do servidor (9 novos, contra Postgres real), 27 do
contrato, 61 de host do firmware, e as telas conferidas num navegador contra
um aquário simulado.
