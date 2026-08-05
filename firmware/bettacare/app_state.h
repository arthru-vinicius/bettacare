#pragma once

#include <Arduino.h>

/**
 * A fronteira entre os dois núcleos.
 *
 * O problema que isto resolve é concreto: no firmware antigo o `loop()` era
 * single-threaded e o handshake TLS do MQTT bloqueava por até 6 segundos.
 * Ou seja, **uma instabilidade de rede travava o botão físico e a histerese da
 * ventoinha**. Trocar MQTT por HTTP não resolveria sozinho — HTTP bloqueia
 * igual.
 *
 * Na reescrita:
 *
 *   núcleo 1 (loop)      controle local: botão, temperatura, ventoinha, RTC
 *   núcleo 0 (net_task)  Wi-Fi, POST de telemetria, OTA
 *
 * Eles não compartilham variáveis soltas. Conversam por três canais, todos
 * definidos aqui: um snapshot protegido por mutex, uma fila de comandos
 * (rede → controle) e uma fila de confirmações (controle → rede).
 *
 * O aquário passa a funcionar com a mesma confiabilidade com ou sem servidor.
 */

// ── Snapshot do estado ──────────────────────────────────────────────────────

enum LightSource : uint8_t {
  LIGHT_SRC_BOOT = 0,
  LIGHT_SRC_SCHEDULE,
  LIGHT_SRC_MANUAL,
  LIGHT_SRC_BUTTON,
  LIGHT_SRC_COMMAND,
};

enum FanModeReport : uint8_t {
  FAN_REPORT_AUTO = 0,
  FAN_REPORT_MANUAL,
  FAN_REPORT_MANUAL_OFF,
  FAN_REPORT_FAILSAFE,
};

struct DeviceSnapshot {
  bool          light_on;
  LightSource   light_source;

  bool          temp_available;
  bool          temp_valid;
  float         temp_celsius;    // só vale se temp_valid
  uint32_t      temp_age_ms;

  bool          fan_on;
  uint8_t       fan_speed_percent;
  uint16_t      fan_rpm;
  FanModeReport fan_mode;

  bool          rtc_available;
  bool          rtc_lost_power;
  char          rtc_time[6];     // "HH:MM"

  int16_t       wifi_rssi;
  char          wifi_ip[16];
  uint16_t      wifi_reconnects;

  uint32_t      uptime_ms;
};

const char* light_source_name(LightSource s);
const char* fan_mode_name(FanModeReport m);

// ── Comandos e confirmações ─────────────────────────────────────────────────

enum CommandKind : uint8_t {
  CMD_LIGHT_SET = 0,
  CMD_FAN_SET_SPEED,
  CMD_FAN_SET_MODE,
  CMD_CONFIG_APPLY,
  CMD_DEVICE_REBOOT,
  CMD_UNKNOWN,
};

/**
 * Toda ação é **estado desejado**, nunca alternância.
 *
 * O `light.toggle` do sistema antigo foi eliminado de propósito: o contrato
 * exige tolerar reenvio, e um toggle reentregue inverteria o estado duas
 * vezes. `light.set{on:true}` reenviado dez vezes tem o mesmo efeito que uma.
 */
struct PendingCommand {
  uint32_t    id;
  CommandKind kind;
  bool        on;          // CMD_LIGHT_SET
  uint8_t     percent;     // CMD_FAN_SET_SPEED
  bool        mode_auto;   // CMD_FAN_SET_MODE
};

static const uint8_t ACK_CODE_LEN = 32;

struct CommandAck {
  uint32_t id;
  bool     ok;
  /** Preenchido só quando `ok` é falso. Ex.: "light.gpio_fault". */
  char     code[ACK_CODE_LEN];
};

// ── API ─────────────────────────────────────────────────────────────────────

void app_state_init();

/** Chamado pelo controle local a cada ciclo do loop. */
void app_state_publish(const DeviceSnapshot& s);

/** Chamado pela task de rede antes de montar o corpo do POST. */
bool app_state_snapshot(DeviceSnapshot& out);

/** Enfileira um comando recebido do servidor. Falso se a fila estiver cheia. */
bool app_state_push_command(const PendingCommand& cmd);

/** O loop consome os comandos pendentes, um por chamada. */
bool app_state_pop_command(PendingCommand& out);

/** O loop confirma (ou recusa) um comando executado. */
bool app_state_push_ack(uint32_t id, bool ok, const char* code);

/** A task de rede drena as confirmações para incluir no próximo POST. */
uint8_t app_state_drain_acks(CommandAck* out, uint8_t max);
