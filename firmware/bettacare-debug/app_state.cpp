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

/**
 * Buffer de acks, não fila FreeRTOS (UPGRADE/03, F1).
 *
 * Uma `xQueue` só permite tirar um item de vez, destrutivamente — não dá para
 * "olhar sem remover". E é exatamente isso que a correção do F1 exige: montar
 * o corpo do POST com os acks pendentes, mas só removê-los da fila **depois**
 * de o servidor confirmar recebimento. Um array pequeno sob mutex, com
 * `peek` + `confirm` separados, dá esse controle.
 */
static CommandAck        _acks[ACK_QUEUE_LEN];
static uint8_t           _ack_count  = 0;
static SemaphoreHandle_t _acks_mutex = nullptr;
/**
 * Contagem de confirmações empilhadas. Só cresce; escrita sob `_acks_mutex`,
 * lida solta (palavra única).
 *
 * A task de rede não enxerga esta direto, e sim a cópia que `publish()` faz:
 * o loop empilha o `ack` em `_drain_commands()` e só publica o retrato no fim
 * da mesma volta. Sem a cópia, o POST antecipado podia sair com "confirmado"
 * e o estado de antes do comando.
 */
static volatile uint32_t _ack_generation = 0;
static volatile uint32_t _ack_generation_publicada = 0;

/**
 * Só o loop escreve (uma vez, no momento do comando) e só a task de rede lê.
 * `volatile` basta: são uma leitura e uma escrita de palavra única cada, sem
 * necessidade de round-trip por mutex para esta cardinalidade de uso.
 */
static volatile bool     _reboot_requested    = false;
static volatile uint32_t _reboot_requested_ms = 0;

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
  _commands       = xQueueCreate(COMMAND_QUEUE_LEN, sizeof(PendingCommand));
  _acks_mutex     = xSemaphoreCreateMutex();
  _ack_count      = 0;
  memset(&_snapshot, 0, sizeof(_snapshot));
  _has_snapshot = false;
}

void app_state_publish(const DeviceSnapshot& s) {
  if (_snapshot_mutex == nullptr) return;
  // Espera curta: o loop nunca pode ficar preso esperando a task de rede. Se o
  // mutex estiver ocupado, o snapshot deste ciclo simplesmente não é publicado
  // e o próximo, 200 ms depois, publica.
  if (xSemaphoreTake(_snapshot_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
  // Lida antes de copiar o retrato: um `ack` empilhado depois desta linha
  // espera a próxima publicação, que já vai refletir o comando dele.
  uint32_t geracao = _ack_generation;
  _snapshot = s;
  _has_snapshot = true;
  _ack_generation_publicada = geracao;
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
  if (_acks_mutex == nullptr) return false;
  if (xSemaphoreTake(_acks_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;

  bool coube = _ack_count < ACK_QUEUE_LEN;
  if (coube) {
    CommandAck& ack = _acks[_ack_count];
    ack.id = id;
    ack.ok = ok;
    if (ok || code == nullptr) {
      ack.code[0] = '\0';
    } else {
      strncpy(ack.code, code, ACK_CODE_LEN - 1);
      ack.code[ACK_CODE_LEN - 1] = '\0';
    }
    _ack_count++;
    _ack_generation = _ack_generation + 1;
  }

  xSemaphoreGive(_acks_mutex);
  return coube;
}

uint8_t app_state_peek_acks(CommandAck* out, uint8_t max) {
  if (_acks_mutex == nullptr) return 0;
  if (xSemaphoreTake(_acks_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return 0;
  uint8_t n = _ack_count < max ? _ack_count : max;
  if (n > 0) memcpy(out, _acks, sizeof(CommandAck) * n);
  xSemaphoreGive(_acks_mutex);
  return n;
}

void app_state_confirm_acks_sent(uint8_t n) {
  if (_acks_mutex == nullptr) return;
  if (xSemaphoreTake(_acks_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

  // Novos acks podem ter entrado entre o `peek` e esta confirmação — como
  // eles só se acrescentam ao final, os `n` mais antigos continuam sendo
  // exatamente os que acabaram de sair no POST bem-sucedido.
  if (n >= _ack_count) {
    _ack_count = 0;
  } else {
    memmove(&_acks[0], &_acks[n], sizeof(CommandAck) * (_ack_count - n));
    _ack_count -= n;
  }

  xSemaphoreGive(_acks_mutex);
}

uint32_t app_state_ack_generation() { return _ack_generation_publicada; }

void app_state_request_reboot() {
  _reboot_requested_ms = millis();
  _reboot_requested = true;
}

bool app_state_reboot_requested() { return _reboot_requested; }

uint32_t app_state_reboot_requested_ms() { return _reboot_requested_ms; }
