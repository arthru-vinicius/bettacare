#pragma once

/**
 * @brief Inicializa WiFiClientSecure (TLS) + PubSubClient, configura LWT,
 *        registra o callback de comandos e tenta a primeira conexão ao HiveMQ.
 *        Deve ser chamado após webserver_init() no setup().
 */
void mqtt_init();

/**
 * @brief Mantém a conexão MQTT ativa (reconecta com backoff de 10 s se necessário)
 *        e processa mensagens recebidas. Não-bloqueante — usa millis().
 *        Chamar no loop() após webserver_loop().
 *        Também publica o estado do sistema se houver mudança desde a última
 *        publicação (publish-on-change), evitando tráfego desnecessário.
 */
void mqtt_loop();

/**
 * @brief Serializa o estado completo do sistema (light, temperature, fan, rtc)
 *        e publica em aquarium/status com retain=true.
 *        Retorna imediatamente se não houver conexão ativa.
 *        Quando force=false, só publica se houver mudança de estado.
 * @param force quando true, publica mesmo sem mudanças detectadas
 */
void mqtt_publish_state(bool force = false);

/**
 * @brief Publica o buffer de logs em aquarium/logs (retain=true) se houver
 *        novas entradas desde a última publicação.
 *        Aplica debounce de 30 s para agrupar a rajada do boot (boot → WiFi →
 *        MQTT em ~15 s) numa única mensagem MQTT.
 *        Chamado automaticamente por mqtt_loop().
 */
void mqtt_publish_logs();
