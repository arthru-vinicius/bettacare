#include "app_state.h"

#include <string.h>

/**
 * Filas pequenas de propósito. Comandos do mesmo alvo se anulam do lado do
 * servidor, então nunca há uma multidão pendente — e memória no ESP32 é
 * escassa. Se encher, é sinal de que o loop travou, e aí a fila é o menor dos
 * problemas.
 */
static const uint8_t COMMAND_QUEUE_LEN = 8;
static const uint8_t ACK_QUEUE_LEN     = 8;

static DeviceSnapshot    _snapshot;
static bool              _has_snapshot = false;
static SemaphoreHandle_t _snapshot_mutex = nullptr;
static QueueHandle_t     _commands = nullptr;
static QueueHandle_t     _acks     = nullptr;

static const char* const LIGHT_SRC_NAMES[] = {"boot", "schedule", "manual",
                                              "button", "command"};
static const char* const FAN_MODE_NAMES[]  = {"auto", "manual", "manual_off",
                                              "failsafe"};

const char* light_source_name(LightSource s) {
  return LIGHT_SRC_NAMES[(uint8_t)s <= LIGHT_SRC_COMMAND ? (uint8_t)s : 0];
}

const char* fan_mode_name(FanModeReport m) {
  return FAN_MODE_NAMES[(uint8_t)m <= FAN_REPORT_FAILSAFE ? (uint8_t)m : 0];
}

void app_state_init() {
  _snapshot_mutex = xSemaphoreCreateMutex();
  _commands = xQueueCreate(COMMAND_QUEUE_LEN, sizeof(PendingCommand));
  _acks     = xQueueCreate(ACK_QUEUE_LEN, sizeof(CommandAck));
  memset(&_snapshot, 0, sizeof(_snapshot));
  _has_snapshot = false;
}

void app_state_publish(const DeviceSnapshot& s) {
  if (_snapshot_mutex == nullptr) return;
  // Espera curta: o loop nunca pode ficar preso esperando a task de rede. Se o
  // mutex estiver ocupado, o snapshot deste ciclo simplesmente não é publicado
  // e o próximo, 200 ms depois, publica.
  if (xSemaphoreTake(_snapshot_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
  _snapshot = s;
  _has_snapshot = true;
  xSemaphoreGive(_snapshot_mutex);
}

bool app_state_snapshot(DeviceSnapshot& out) {
  if (_snapshot_mutex == nullptr) return false;
  if (xSemaphoreTake(_snapshot_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
  bool ok = _has_snapshot;
  if (ok) out = _snapshot;
  xSemaphoreGive(_snapshot_mutex);
  return ok;
}

bool app_state_push_command(const PendingCommand& cmd) {
  if (_commands == nullptr) return false;
  return xQueueSend(_commands, &cmd, 0) == pdTRUE;
}

bool app_state_pop_command(PendingCommand& out) {
  if (_commands == nullptr) return false;
  return xQueueReceive(_commands, &out, 0) == pdTRUE;
}

bool app_state_push_ack(uint32_t id, bool ok, const char* code) {
  if (_acks == nullptr) return false;

  CommandAck ack;
  ack.id = id;
  ack.ok = ok;
  if (ok || code == nullptr) {
    ack.code[0] = '\0';
  } else {
    strncpy(ack.code, code, ACK_CODE_LEN - 1);
    ack.code[ACK_CODE_LEN - 1] = '\0';
  }

  return xQueueSend(_acks, &ack, 0) == pdTRUE;
}

uint8_t app_state_drain_acks(CommandAck* out, uint8_t max) {
  if (_acks == nullptr) return 0;
  uint8_t n = 0;
  while (n < max && xQueueReceive(_acks, &out[n], 0) == pdTRUE) n++;
  return n;
}
