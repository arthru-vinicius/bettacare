# Testes de host do firmware

Testes que rodam **no PC**, sem ESP32: cada `test_*.cpp` inclui o `.cpp` real
de `firmware/bettacare` e o compila com o `g++` comum, trocando o hardware por
stubs mínimos (`stubs/`). O que está sob teste é o código de produção, linha
por linha — não uma reimplementação dele.

```sh
sh firmware/test/run.sh
```

Precisa de um `g++` com C++17 no `PATH`. No Windows desta máquina é o MinGW em
`C:\mingw64\bin`; no Linux, o `g++` do sistema. Os binários vão para
`firmware/test/build/`, que o `.gitignore` já cobre.

## O que cobrem

| Teste | Arquivo do firmware | Cenários |
|---|---|---|
| `test_rtc.cpp` | `rtc_manager.cpp` | Automação no boot; leitura de hora válida porém errada sem trocar a luz; virada real confirmada na leitura seguinte; três falhas de I²C derrubando o módulo; sem NTP a automação espera, com NTP segue pela hora do ESP32; módulo reencontrado pela sondagem; bytes inválidos (o "40:08") descartados; sincronização por NTP; reaplicação imediata e config nova confirmada |
| `test_feeder.cpp` | `feeder_link.cpp` | `PONG`, `SCHEDULE` e `FED` válidos; campos fora da faixa do contrato descartando a linha inteira; dígito corrompido; idade `-1` e idade absurda; idade envelhecendo entre `PONG`s; timeout, lixo que não reconecta, reconexão; linha gigante e CRLF; campo mais longo que o buffer não virando dois campos; formato de `FEED`/`CFG` |

Foi rodando estes testes que apareceu o bug do campo longo partido em dois no
enlace do alimentador — o teste ao vivo não pegaria, porque o módulo ainda não
existe.

## Como funcionam os stubs

| Stub | Substitui | Controle que o teste ganha |
|---|---|---|
| `Arduino.h` | core do Arduino-ESP32 | `millis()` avança por `g_millis`; `String` mínima; `Serial`/`Serial2` falsos com entrada injetável e saída capturada; mutex que sempre concede |
| `Wire.h` | barramento I²C | registradores de minuto e hora do DS3231, erro de transação programável |
| `RTClib.h` | RTClib | `begin()`/`lostPower()`/`adjust()` sobre o `Wire` falso |
| `esp_task_wdt.h`, `driver/gpio.h` | ESP-IDF | nada a controlar — só existem para compilar |

O resto do que o `.cpp` sob teste chama de outros módulos (`event_log`,
`light_set`, `device_config_snapshot`…) é definido no próprio teste, que
registra as chamadas para conferir depois — é assim que um teste sabe que a
luz trocou, ou que um evento saiu.

## Acrescentando um teste

Um módulo entra aqui quando a lógica dele é decisiva e as dependências de
hardware são poucas. O caminho é o mesmo dos dois existentes: criar
`test_<modulo>.cpp`, incluir `"<modulo>.cpp"`, definir o que ele chama dos
outros módulos e, se ele tocar hardware novo, acrescentar só o stub mínimo
para compilar. O `run.sh` acha o arquivo novo sozinho.

Ao mudar o firmware, rode os testes **antes** de gravar no ESP32: são segundos,
contra minutos de compilação e gravação por OTA.
