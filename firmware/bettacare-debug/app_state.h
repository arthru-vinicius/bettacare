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
  bool          wifi_rssi_valid;  // falso se não havia associação no instante da leitura
  char          wifi_ip[16];
  uint16_t      wifi_reconnects;

  uint32_t      uptime_ms;

  // Diagnóstico do controlador, acrescentado na rodada de confiabilidade de
  // 2026-08-21 (UPGRADE/03, F7). Só os campos que nascem no núcleo de
  // controle; heap, motivo do reset e latência do POST são medidos
  // diretamente pela task de rede em `api_client.cpp`, que já roda no núcleo
  // certo para isso.
  bool          button_pressed;
  uint16_t      pot_raw_adc;
  uint32_t      tach_pulses_raw;

  // Módulo opcional de alimentação de precisão, ligado por UART2 (ver
  // `feeder_link.h`). `feeder_connected == false` é o estado normal quando o
  // módulo não existe ou está desligado de propósito — não é falha.
  bool          feeder_connected;
  bool          feeder_auto_enabled;
  uint8_t       feeder_hour1;
  uint8_t       feeder_hour2;
  uint8_t       feeder_grains_per_feeding;
  /** Segundos desde a última alimentação. `UINT32_MAX` = nunca, desde que soubemos do módulo. */
  uint32_t      feeder_last_feed_age_s;
  uint8_t       feeder_last_feed_requested;
  uint8_t       feeder_last_feed_confirmed;
  bool          feeder_last_feed_ok;
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
  CMD_DEVICE_DIAGNOSE,
  CMD_FEEDER_FEED_NOW,
  CMD_FEEDER_SET_CONFIG,
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

  // CMD_FEEDER_FEED_NOW usa só `feeder_grains` (0 = usa o padrão do módulo).
  // CMD_FEEDER_SET_CONFIG usa os quatro campos.
  uint8_t     feeder_hour1;
  uint8_t     feeder_hour2;
  uint8_t     feeder_grains;
  bool        feeder_auto_enabled;
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

/**
 * Copia até `max` confirmações pendentes **sem removê-las** — para montar o
 * corpo do POST. Se a requisição falhar, elas continuam lá para o próximo
 * ciclo tentar de novo (UPGRADE/03, F1): um `ack` perdido faz a interface
 * dizer "comando expirou" para uma ação que na verdade funcionou.
 */
uint8_t app_state_peek_acks(CommandAck* out, uint8_t max);

/** Remove as `n` confirmações mais antigas — só depois de um POST aceito (`200`). */
void app_state_confirm_acks_sent(uint8_t n);

/**
 * Quantas confirmações já foram empilhadas desde o boot, **na última
 * publicação do retrato** — o valor só avança junto com um retrato que já
 * reflete os comandos confirmados. A task de rede compara com o do último POST
 * para mandar a confirmação na hora, em vez de esperar o próximo ciclo — ver
 * `net_task.cpp`.
 */
uint32_t app_state_ack_generation();

// ── Reboot pendente ──────────────────────────────────────────────────────────

/**
 * Sinaliza que um `device.reboot` foi confirmado e está esperando a
 * confirmação **sair** antes de reiniciar (UPGRADE/03, F2).
 *
 * Reiniciar por temporizador fixo (como antes) tipicamente reinicia antes de
 * o `ack` alcançar o próximo POST — o intervalo padrão de telemetria já é
 * maior que a espera fixa, e o recuo progressivo pode ser bem maior ainda.
 * Em vez disso, o loop só marca a intenção; quem decide o momento certo é a
 * task de rede, depois de um POST que realmente levou o `ack`.
 */
void app_state_request_reboot();

/** Verdadeiro entre o pedido e o reinício efetivo. */
bool app_state_reboot_requested();

/** `millis()` de quando o reinício foi pedido — base do teto de segurança. */
uint32_t app_state_reboot_requested_ms();
