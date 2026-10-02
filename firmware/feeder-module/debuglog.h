#pragma once
#include <Arduino.h>

/**
 * O log do módulo, que precisa chegar a alguém mesmo sem o cabo USB:
 *
 * - **USB**, como sempre;
 * - **RAM que sobrevive a reinício** — as últimas ~3 KB de linhas, numeradas,
 *   em `GET /log`. Um reinício por *watchdog*, exceção ou queda de tensão não
 *   apaga o que veio antes dele; só faltar energia apaga;
 * - **UDP**, uma linha por datagrama, para o PC da bancada — com
 *   `DEBUG_LOG_HOST` no `config.h` (ver `tools/udplog.mjs`).
 *
 * Use `Log.printf`/`Log.println` no lugar de `Serial`.
 */
class DebugLog : public Print {
 public:
  size_t write(uint8_t c) override;
  size_t write(const uint8_t* buf, size_t len) override;
};

extern DebugLog Log;

/** A primeira coisa depois do `hw_init()`: antes dela, o log só vai para a USB. */
void log_init();

/** As linhas guardadas com número maior que `since` (0 = todas), em ordem. */
String log_recent(uint32_t since);
/**
 * O número que a próxima linha vai ter. Menor que o último que alguém já leu:
 * o módulo ficou sem energia e o log recomeçou do 1.
 */
uint32_t log_next_seq();

/** Por que o módulo reiniciou da última vez, em português. */
const char* log_reset_reason();
