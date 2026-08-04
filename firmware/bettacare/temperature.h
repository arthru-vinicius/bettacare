#pragma once

/**
 * @brief Inicializa o barramento OneWire e o sensor DS18B20.
 *        Configura resolução de 12 bits e modo de conversão não-bloqueante.
 *        Detecta presença do sensor e imprime resultado no Serial.
 */
void temperature_init();

/**
 * @brief Gerencia o ciclo de conversão do DS18B20 de forma não-bloqueante.
 *        Deve ser chamado no loop(). Inicia uma nova conversão quando a
 *        anterior terminar (~750 ms) e atualiza o valor em cache.
 */
void temperature_update();

/**
 * @brief Retorna a última temperatura lida pelo DS18B20, em graus Celsius.
 *        Não bloqueia — retorna o valor em cache da última conversão concluída.
 * @return temperatura em °C, ou -127,0 se o sensor não estiver disponível
 *         ou se nenhuma conversão tiver sido concluída ainda.
 */
float temperature_read();

/**
 * @brief Retorna true se o sensor DS18B20 foi encontrado durante a inicialização.
 */
bool temperature_available();

/**
 * @brief Retorna true quando já existe pelo menos uma leitura válida em cache.
 */
bool temperature_has_valid_reading();

/**
 * @brief Retorna true quando a última leitura válida ainda está recente.
 *        Leituras antigas/stale são tratadas como indisponíveis para automação.
 */
bool temperature_is_fresh();
