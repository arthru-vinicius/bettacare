#pragma once
#include <Arduino.h>

/**
 * @brief Conecta ao Wi-Fi com as credenciais e IP estático definidos em config.h.
 *        Inicia tentativa de conexão de forma não-bloqueante.
 */
void wifi_connect();

/**
 * @brief Verifica se o Wi-Fi ainda está conectado. Deve ser chamado no loop().
 *        Reconecta automaticamente sem bloquear as demais funcionalidades.
 */
void wifi_check_reconnect();

/**
 * @brief Retorna true se o ESP32 estiver conectado ao Wi-Fi.
 */
bool wifi_is_connected();

/**
 * @brief Informa se o modo de recuperação (AP de configuração) está ativo.
 */
bool wifi_recovery_ap_active();

/**
 * @brief Retorna o SSID do Access Point de recuperação.
 */
String wifi_recovery_ap_ssid();

/**
 * @brief Retorna o SSID atualmente configurado para conexão STA.
 */
String wifi_configured_ssid();

/**
 * @brief Atualiza e persiste credenciais Wi-Fi (NVS), iniciando nova tentativa.
 * @param ssid SSID da rede (1..32 chars)
 * @param password senha da rede (0 ou 8..63 chars)
 * @return true se credenciais válidas e aceitas; false em caso de validação inválida
 */
bool wifi_set_credentials(const String &ssid, const String &password);

/**
 * @brief Quedas de conexão desde o boot. Subindo depressa, o link está instável
 *        — é o que o servidor usa para classificar o Wi-Fi como degradado.
 */
uint16_t wifi_reconnect_count();

/** @brief RSSI atual em dBm. Só é medição real quando `wifi_rssi_valid()` também é verdadeiro. */
int16_t wifi_rssi();

/**
 * @brief Falso quando não há associação — o -120 de `wifi_rssi()` é um valor
 *        de repouso, não uma leitura. Sem isto, um snapshot capturado durante
 *        uma queda breve de Wi-Fi podia viajar como se fosse RSSI real, e o
 *        servidor classificaria o link como "fraco" quando na verdade não
 *        havia leitura nenhuma (UPGRADE/03, F13).
 */
bool wifi_rssi_valid();

/** @brief IP local, ou string vazia se desconectado. */
String wifi_local_ip();
