#pragma once
#include <Arduino.h>

/**
 * @brief Conecta ao Wi-Fi com as credenciais definidas em config.h.
 *        Inicia tentativa de conexão de forma não-bloqueante.
 */
void wifi_connect();

/**
 * @brief Verifica se o Wi-Fi ainda está conectado. Deve ser chamado no loop().
 *        Reconecta automaticamente sem bloquear as demais funcionalidades.
 */
void wifi_check_reconnect();

/**
 * @brief Retorna true se o módulo estiver conectado ao Wi-Fi.
 */
bool wifi_is_connected();

/**
 * @brief Quedas de conexão desde o boot.
 */
uint16_t wifi_reconnect_count();
