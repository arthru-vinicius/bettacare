#pragma once
#include <Arduino.h>

/**
 * @brief Inicializa o buffer circular de logs.
 *        Deve ser chamado logo após Serial.begin(), antes de qualquer log_event().
 */
void log_init();

/**
 * @brief Registra um evento no buffer circular.
 *        Ignora silenciosamente se a mensagem for idêntica à última inserida
 *        (deduplicação simples para evitar spam de erros repetidos).
 *        Use apenas para eventos de boot, erros e recuperações —
 *        não para operação rotineira (luz ligada, ventoinha, etc.).
 * @param msg  Mensagem do evento (ex: "[WiFi] Conexao perdida")
 */
void log_event(const char* msg);

/**
 * @brief Variante printf-style de log_event.
 * @param fmt  Formato printf
 * @param ...  Argumentos
 */
void log_eventf(const char* fmt, ...);

/**
 * @brief Serializa o buffer para JSON ordenado do mais antigo ao mais recente.
 * @return String JSON (ex: [{"t":"10:32","m":"[Sistema] Boot completo"}])
 *         ou "[]" se o buffer estiver vazio.
 */
String log_get_json();

/**
 * @brief Limpa o buffer e reinicia a deduplicação.
 *        Marca pending=true para que o estado vazio seja republicado no MQTT.
 */
void log_clear();

/**
 * @brief Retorna true se houve novas entradas desde a última publicação MQTT.
 *        Usado por mqtt_manager para decidir se deve republicar aquarium/logs.
 */
bool log_has_pending();

/**
 * @brief Marca o estado atual como "já publicado".
 *        Chamado por mqtt_manager após publicação bem-sucedida.
 */
void log_mark_published();
