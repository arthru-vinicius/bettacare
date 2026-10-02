#include "diagnostics.h"

#include <DallasTemperature.h>
#include <OneWire.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <math.h>

#include "app_state.h"
#include "config.h"
#include "device_config.h"
#include "fan.h"
#include "light.h"
#include "rtc_manager.h"
#include "temperature.h"
#include "web_server.h"
#include "wifi_manager.h"

/** Namespace e chave usados só pelo teste de escrita — nunca toca a config real. */
#define DIAG_NVS_NS  "bc_diag"
#define DIAG_NVS_KEY "probe"

static DiagnosticReport  _report;
static SemaphoreHandle_t _mutex = nullptr;

void diagnostics_init() {
  _mutex = xSemaphoreCreateMutex();
  _report.valid = false;
  _report.count = 0;
}

// ── Montagem do relatório ────────────────────────────────────────────────────

static void _add(DiagnosticReport& r, EventComponent comp, const char* status,
                 bool probed, const char* fmt, ...) {
  if (r.count >= DIAG_MAX_CHECKS) return;

  DiagnosticCheck& c = r.checks[r.count];
  c.comp   = comp;
  c.probed = probed;
  strncpy(c.status, status, sizeof(c.status) - 1);
  c.status[sizeof(c.status) - 1] = '\0';

  va_list args;
  va_start(args, fmt);
  vsnprintf(c.detail, sizeof(c.detail), fmt, args);
  va_end(args);

  r.count++;
}

// ── Sondas ───────────────────────────────────────────────────────────────────

/**
 * Varre o barramento 1-Wire de verdade, em vez de perguntar ao cache.
 *
 * É o teste que responde "o sensor está aí agora?", e não "estava da última
 * vez que alguém olhou" — a distinção que importa quando o cabo tem mau
 * contato e o estado oscila.
 */
static void _probe_temp(DiagnosticReport& r) {
  OneWire bus(PIN_DS18B20);
  DallasTemperature sensores(&bus);
  sensores.begin();

  int n = sensores.getDeviceCount();
  if (n == 0) {
    _add(r, COMP_TEMP, "missing", true,
         "Nenhum DS18B20 respondeu no barramento 1-Wire (GPIO %d). "
         "Verifique o cabo e o resistor de pull-up de 4,7k.",
         PIN_DS18B20);
    return;
  }

  // Conversão completa, bloqueante: é diagnóstico, não caminho quente.
  sensores.setResolution(12);
  sensores.requestTemperatures();
  float t = sensores.getTempCByIndex(0);

  if (t <= -100.0f || isnan(t)) {
    _add(r, COMP_TEMP, "fault", true,
         "%d sensor(es) no barramento, mas a leitura voltou invalida (%.1f C).", n, t);
    return;
  }
  if (t < 0.0f || t > 45.0f) {
    _add(r, COMP_TEMP, "degraded", true,
         "Leitura de %.1f C fora da faixa plausivel para aquario, com %d sensor(es).", t, n);
    return;
  }

  _add(r, COMP_TEMP, "ok", true, "%d sensor(es) no barramento, leitura de %.1f C.", n, t);
}

/** Procura o DS3231 no endereço fixo e confere se a hora faz sentido. */
static void _probe_rtc(DiagnosticReport& r) {
  // Pelo mutex do I²C (UPGRADE/07): antes isto falava com o barramento direto,
  // e podia cruzar com o NTP ajustando o DS3231 pelo outro núcleo.
  bool respondeu = rtc_probe_present();

  if (!respondeu) {
    _add(r, COMP_RTC, "missing", true,
         "DS3231 nao respondeu em 0x68. Verifique SDA (GPIO 21) e SCL (GPIO 22).");
    return;
  }

  String hora = rtc_get_time_str();
  if (hora == "--:--") {
    _add(r, COMP_RTC, "degraded", true,
         "Modulo respondeu no I2C, mas a hora ainda nao foi lida.");
    return;
  }
  if (rtc_lost_power()) {
    _add(r, COMP_RTC, "degraded", true,
         "Modulo respondeu e marca %s, mas acusa perda de energia — troque a bateria CR2032.",
         hora.c_str());
    return;
  }

  _add(r, COMP_RTC, "ok", true, "DS3231 respondeu em 0x68, hora %s.", hora.c_str());
}

/**
 * Escreve e relê uma chave própria — o único teste que prova que a flash
 * ainda aceita gravação, que é o que `nvs.write_failed` alega quando falha.
 *
 * ## Por que a escrita é limitada por tempo
 *
 * A flash do ESP32 tem ciclos de escrita finitos (~100 mil por setor, com
 * *wear leveling* da NVS por cima). Um diagnóstico manual ocasional não chega
 * perto disso — dez por dia dariam 27 anos. O risco real é outro: **alguém
 * automatizar a chamada.** Um diagnóstico por minuto passaria de meio milhão
 * de escritas por ano, e aí o autoteste vira a causa do defeito que ele
 * deveria detectar.
 *
 * A janela de uma hora resolve sem perder valor: entre um teste de escrita e
 * outro, o diagnóstico ainda **lê** a chave e confirma que a flash responde e
 * que o dado gravado continua íntegro — o que já pega corrupção. A escrita
 * completa, que é a parte que desgasta, acontece no máximo 24 vezes por dia.
 */
static const uint32_t NVS_WRITE_TEST_INTERVAL_MS = 60UL * 60UL * 1000UL;
static uint32_t _last_nvs_write_test_ms = 0;
static bool     _nvs_write_tested       = false;

static void _probe_nvs(DiagnosticReport& r) {
  uint16_t falhas = nvs_failure_count();
  uint32_t agora = (uint32_t)millis();

  bool pode_escrever =
      !_nvs_write_tested || (agora - _last_nvs_write_test_ms) >= NVS_WRITE_TEST_INTERVAL_MS;

  if (!pode_escrever) {
    // Só leitura: confirma que a flash responde e que o valor sobreviveu.
    Preferences somenteLeitura;
    if (!somenteLeitura.begin(DIAG_NVS_NS, true)) {
      nvs_report_failure();
      _add(r, COMP_NVS, "fault", true,
           "Nao foi possivel abrir a NVS nem para leitura.");
      return;
    }
    uint32_t lido = somenteLeitura.getUInt(DIAG_NVS_KEY, 0);
    somenteLeitura.end();

    if (lido == 0) {
      _add(r, COMP_NVS, "degraded", true,
           "Leitura da chave de teste voltou vazia — o dado gravado nao sobreviveu.");
      return;
    }
    _add(r, COMP_NVS, falhas > 0 ? "degraded" : "ok", true,
         "Leitura da NVS integra%s. Teste de escrita e feito no maximo 1x/h para "
         "poupar ciclos da flash.",
         falhas > 0 ? ", mas houve falha(s) de gravacao desde o boot" : "");
    return;
  }

  uint32_t marca = agora == 0 ? 1 : agora;   // nunca zero: zero é o sentinela de "vazio"
  _last_nvs_write_test_ms = agora;
  _nvs_write_tested = true;

  Preferences prefs;
  if (!prefs.begin(DIAG_NVS_NS, false)) {
    nvs_report_failure();
    _add(r, COMP_NVS, "fault", true,
         "Nao foi possivel abrir a NVS para escrita. A configuracao nao sobrevive a um reboot.");
    return;
  }

  prefs.putUInt(DIAG_NVS_KEY, marca);
  uint32_t lido = prefs.getUInt(DIAG_NVS_KEY, 0);
  prefs.end();

  if (lido != marca) {
    nvs_report_failure();
    _add(r, COMP_NVS, "fault", true,
         "Escrita de teste nao voltou integra na leitura (%lu != %lu). Flash possivelmente desgastada.",
         (unsigned long)lido, (unsigned long)marca);
    return;
  }

  if (falhas > 0) {
    _add(r, COMP_NVS, "degraded", true,
         "Escrita e leitura de teste passaram, mas houve %u falha(s) de NVS desde o boot.",
         (unsigned)falhas);
    return;
  }

  _add(r, COMP_NVS, "ok", true, "Escrita e leitura de teste passaram.");
}

/** Lê o ADC do potenciômetro e checa se o valor é plausível. */
static void _probe_pot(DiagnosticReport& r) {
  // Média de algumas leituras: o ADC do ESP32 é ruidoso, e uma amostra só
  // levaria a conclusões sobre ruído em vez de sobre o potenciômetro.
  uint32_t soma = 0;
  for (int i = 0; i < 8; i++) {
    soma += analogRead(PIN_POT);
    delay(2);
  }
  int adc = (int)(soma / 8);

  if (adc >= 4090) {
    _add(r, COMP_POT, "degraded", true,
         "ADC saturado no topo (%d/4095). Pode ser fim de curso ou saturacao do conversor.", adc);
    return;
  }
  if (adc <= 2) {
    _add(r, COMP_POT, "degraded", true,
         "ADC colado no zero (%d/4095). Pode ser fim de curso ou wiper sem contato.", adc);
    return;
  }

  _add(r, COMP_POT, "ok", true, "Leitura estavel em %d/4095 (GPIO %d).", adc, PIN_POT);
}

/** Lê o pino do botão. Preso em LOW em repouso é mau contato. */
static void _probe_button(DiagnosticReport& r) {
  bool pressionado = light_button_pressed();
  if (pressionado) {
    _add(r, COMP_BUTTON, "degraded", true,
         "Pino lido como pressionado durante o teste. Se ninguem esta tocando, verifique o contato.");
    return;
  }
  _add(r, COMP_BUTTON, "ok", true, "Pino em repouso (nivel alto), como esperado.");
}

/**
 * Ventoinha: observação, não sondagem.
 *
 * Girá-la para testar mudaria o estado do aquário, que é justamente o que este
 * diagnóstico não faz. O que dá para afirmar com honestidade é a coerência
 * entre o PWM comandado e o que o tacômetro devolve.
 */
static void _probe_fan(DiagnosticReport& r) {
  int duty = fan_get_speed_percent();
  int rpm  = fan_get_rpm();

  if (duty == 0) {
    _add(r, COMP_FAN, "unknown", false,
         "Ventoinha parada (PWM em 0%%), entao o tacometro em %d rpm e o esperado. "
         "Nao da para testar sem liga-la.", rpm);
    return;
  }
  if (rpm == 0) {
    _add(r, COMP_FAN, "fault", false,
         "PWM em %d%% e tacometro em zero: a ventoinha nao esta girando. "
         "Cabo solto, rolamento travado ou fonte de 12V caida.", duty);
    return;
  }

  _add(r, COMP_FAN, "ok", false, "PWM em %d%% com %d rpm medidos — coerente.", duty, rpm);
}

/**
 * Luminária: o limite estrutural do sistema.
 *
 * O SSR não tem realimentação. O firmware sabe o que **comandou** ao GPIO, não
 * o que a lâmpada fez. Relatar isso como "ok" seria mentir; o honesto é dizer
 * o que se sabe e o que não se sabe.
 */
static void _probe_light(DiagnosticReport& r) {
  bool ligada = light_get_state();
  _add(r, COMP_LIGHT, "unknown", false,
       "GPIO %d comandado para %s. O SSR nao tem retorno, entao nao ha como confirmar "
       "daqui se a lampada obedeceu — confira visualmente.",
       PIN_SSR, ligada ? "ACESO" : "APAGADO");
}

/** Rede: associação e qualidade do sinal. */
static void _probe_wifi(DiagnosticReport& r) {
  if (!wifi_is_connected()) {
    _add(r, COMP_WIFI, "missing", true, "Sem associacao Wi-Fi no momento do teste.");
    return;
  }

  int16_t rssi = wifi_rssi();
  uint16_t reconexoes = wifi_reconnect_count();

  if (rssi < -80) {
    _add(r, COMP_WIFI, "degraded", true,
         "Conectado, mas sinal fraco (%d dBm) e %u reconexao(oes) desde o boot.",
         (int)rssi, (unsigned)reconexoes);
    return;
  }

  _add(r, COMP_WIFI, "ok", true, "Conectado com %d dBm, %u reconexao(oes) desde o boot.",
       (int)rssi, (unsigned)reconexoes);
}

/** Memória e motivo do último reset — a saúde do próprio controlador. */
static void _probe_system(DiagnosticReport& r) {
  uint32_t livre = (uint32_t)esp_get_free_heap_size();
  uint32_t minimo = (uint32_t)esp_get_minimum_free_heap_size();
  uint32_t maior = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  esp_reset_reason_t motivo = esp_reset_reason();

  if (motivo == ESP_RST_BROWNOUT) {
    _add(r, COMP_SYSTEM, "fault", true,
         "Ultimo reinicio foi por queda de tensao (brownout). Verifique a alimentacao "
         "e o capacitor de reservatorio no trilho 3V3.");
    return;
  }
  if (motivo == ESP_RST_PANIC || motivo == ESP_RST_TASK_WDT ||
      motivo == ESP_RST_INT_WDT || motivo == ESP_RST_WDT) {
    _add(r, COMP_SYSTEM, "fault", true,
         "Ultimo reinicio foi por falha de software (panic ou watchdog). Heap livre: %lu bytes.",
         (unsigned long)livre);
    return;
  }
  if (livre < 40000UL) {
    _add(r, COMP_SYSTEM, "degraded", true,
         "Heap livre baixo: %lu bytes (minimo desde o boot: %lu).",
         (unsigned long)livre, (unsigned long)minimo);
    return;
  }
  // Muito heap livre com bloco contíguo pequeno é o retrato de fragmentação —
  // e é uma condição que só aparece olhando os dois números juntos.
  if (maior < livre / 3) {
    _add(r, COMP_SYSTEM, "degraded", true,
         "Heap fragmentado: %lu bytes livres, mas o maior bloco tem so %lu.",
         (unsigned long)livre, (unsigned long)maior);
    return;
  }

  _add(r, COMP_SYSTEM, "ok", true,
       "Heap livre %lu bytes (minimo %lu, maior bloco %lu). Ultimo reinicio sem falha.",
       (unsigned long)livre, (unsigned long)minimo, (unsigned long)maior);
}

/** Última atualização remota — persistida, sobrevive ao reboot que o OTA causa. */
static void _probe_ota(DiagnosticReport& r) {
  const char* resultado = webserver_ota_last_result();

  if (strcmp(resultado, "failed") == 0) {
    _add(r, COMP_OTA, "fault", false,
         "A ultima atualizacao remota falhou. O dispositivo seguiu com o firmware anterior.");
    return;
  }
  if (strcmp(resultado, "ok") == 0) {
    _add(r, COMP_OTA, "ok", false, "A ultima atualizacao remota foi concluida com sucesso.");
    return;
  }
  _add(r, COMP_OTA, "ok", false, "Nenhuma atualizacao remota registrada neste dispositivo.");
}

// ── API ──────────────────────────────────────────────────────────────────────

void diagnostics_run(uint32_t command_id) {
  uint32_t inicio = millis();

  DiagnosticReport r;
  r.valid = true;
  r.command_id = command_id;
  r.ran_at_uptime_ms = inicio;
  r.count = 0;

  // Ordem pensada para o relatório: hardware do aquário primeiro, que é o que
  // o usuário quer ver, e infraestrutura depois.
  _probe_temp(r);
  _probe_rtc(r);
  _probe_fan(r);
  _probe_light(r);
  _probe_pot(r);
  _probe_button(r);
  _probe_wifi(r);
  _probe_nvs(r);
  _probe_system(r);
  _probe_ota(r);

  r.duration_ms = millis() - inicio;

  if (_mutex != nullptr && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    _report = r;
    xSemaphoreGive(_mutex);
  }

  uint8_t problemas = 0;
  for (uint8_t i = 0; i < r.count; i++) {
    if (strcmp(r.checks[i].status, "ok") != 0 &&
        strcmp(r.checks[i].status, "unknown") != 0) {
      problemas++;
    }
  }

  event_log(problemas > 0 ? SEV_WARN : SEV_INFO, COMP_SYSTEM,
            problemas > 0 ? "system.diagnostic_found_problem" : "system.diagnostic_ran",
            "Autodiagnostico: %u verificacoes em %lums, %u problema(s)",
            (unsigned)r.count, (unsigned long)r.duration_ms, (unsigned)problemas);
}

bool diagnostics_take(DiagnosticReport& out) {
  if (_mutex == nullptr) return false;
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;

  bool tem = _report.valid;
  if (tem) {
    out = _report;
    _report.valid = false;
  }

  xSemaphoreGive(_mutex);
  return tem;
}
