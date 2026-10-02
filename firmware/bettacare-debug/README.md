# Firmware de debug do controlador

O firmware de produção (`firmware/bettacare`) inteiro, **mais** a instrumentação
que achou o relé que mudava de estado sozinho (UPGRADE/07) e que depois validou,
injetando falhas ao vivo, as correções de estabilidade de 2026-10-01.

Use para investigar. Para o uso normal, grave o de produção.

## Como gravar

Igual ao de produção, só muda a pasta:

1. Copie `config.example.h` para `config.h` nesta pasta e preencha com os mesmos
   valores do `config.h` de produção. O `config.h` fica fora do git
   (`firmware/*/config.h` no `.gitignore`). Mantenha o `FW_VERSION` com o sufixo
   `-debug`: é assim que a aba Saúde do app mostra que é este firmware que está
   rodando.
2. Compile e grave por OTA em `http://<IP do ESP32>/update` (login e senha de
   OTA do `config.h`), ou pelo USB. Para voltar, grave o de produção do mesmo
   jeito.

```sh
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/bettacare-debug
```

## O endpoint `/debug`

`GET http://<IP>/debug`, com o mesmo token da API local (`X-Api-Token`, ou
`?token=`). Sem parâmetros só lê; com parâmetros, aplica e devolve o estado.

O retorno traz:

- **Contadores**: bordas que chegaram no pino do botão (`btn_edges`), trocas da
  luz por origem (botão, horário, comando), divergências entre o GPIO23 e o que
  o firmware escreveu, último código HTTP do POST, resultado da configuração do
  watchdog e motivo do último reset.
- **Memória**: heap livre, mínimo desde o boot, maior bloco alocável, e a menor
  folga que a pilha do loop já teve (`loop_stack_hwm`, em bytes, de 8192).
- **`events`**: os últimos 16 eventos de `event_log()`, para ver o que o
  firmware registrou sem servidor e sem cabo.
- **`ring`**: anel de 128 marcas com carimbo de tempo (1-Wire, I²C, POST,
  pressões do botão, trocas da luz).

### Chaves de experimento

| Parâmetro | Efeito |
|---|---|
| `temp=0` / `temp=1` | desliga / religa o DS18B20 |
| `temp_ms=N` | intervalo entre conversões, de 1000 a 60000 ms (o normal é 5000) |
| `rtc=0` / `rtc=1` | desliga / religa a leitura do DS3231 e a automação por horário |
| `feeder=0` / `feeder=1` | desliga / religa o enlace UART2 do alimentador |

Foi desligando um subsistema por vez que a causa do relé apareceu: com o DS18B20
desligado, zero picos no botão; com ele a 7 s em vez de 5 s, o "fantasma" mudou
de ritmo junto.

### Injeção de falhas

Cada contador é consumido uma leitura por vez, e depois tudo volta ao normal.

| Parâmetro | Simula | O que deve acontecer |
|---|---|---|
| `temp85=N` | o DS18B20 devolvendo 85,0 °C (valor de power-on) nas próximas N leituras | 1 leitura: `temp.reset_value`, a leitura é descartada e a ventoinha não reage. 3 seguidas: sensor perdido, ventoinha em modo de segurança, e reencontrado em ~10 s |
| `rtc_bad=N` | o DS3231 devolvendo bytes inválidos (0x40:0x08, o "40:08" visto em campo) | `rtc.bad_read` e a hora não muda; 3 seguidas derrubam o módulo |
| `rtc_fail=N` | as próximas N leituras falhando no I²C | com 3 seguidas, `rtc.missing`, e a hora passa a vir do relógio interno do ESP32 (NTP); o módulo é reencontrado na sondagem seguinte, a cada 15 s |
| `rtc_offset=MIN&rtc_offset_reads=N` | N leituras válidas, porém deslocadas MIN minutos | com 1 leitura, a luz **não** muda; com 2 ou mais, a virada acontece na segunda leitura |
| `diag=1` | — | roda o autodiagnóstico agora (sem precisar do servidor) |
| `hang=1` | trava o loop de propósito | o resto do sistema segue respondendo, e o watchdog reinicia o chip em até 60 s, com `reset_reason` 6 |

Atenção: `hang=1` deixa no servidor um reinício por watchdog, que aparece na aba
Saúde como falha até o próximo boot normal.

## Resultado da última rodada (2026-10-01, em bancada)

- 32 min de observação com o DS18B20 gerando 62.909 picos no GPIO18: 4 trocas
  da luz, todas de toques reais.
- 85 °C isolado descartado; três seguidas levaram ao modo de segurança e à
  recuperação sozinha.
- Leitura inválida e falha de I²C do DS3231: hora preservada, módulo derrubado
  na 3ª falha, hora seguindo pelo NTP no fuso certo, módulo reencontrado.
- Leitura de hora errada isolada não trocou a luz; repetida, trocou na 2ª
  leitura e voltou quando a hora real foi confirmada.
- Autodiagnóstico: 163 ms; pilha do loop no máximo a ~3,6 KB dos 8 KB.
- `hang=1`: reinício pelo watchdog em ~60 s, `reset_reason` 6.

## Mantendo em dia com a produção

Esta pasta é uma cópia. Quando o firmware de produção mudar e for preciso
investigar de novo, copie os `.cpp`/`.h` de `firmware/bettacare` para cá e
reaplique as linhas de instrumentação. Elas são poucas e todas citam
`debug_probe.h` ou uma variável `dbg_*`:

```sh
grep -n "dbg_\|debug_probe" firmware/bettacare-debug/*.cpp firmware/bettacare-debug/*.ino
```
