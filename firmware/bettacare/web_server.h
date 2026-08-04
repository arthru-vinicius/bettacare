#pragma once

/**
 * @brief Configura os headers CORS globais, registra todos os endpoints REST,
 *        inicia o ElegantOTA e sobe o servidor HTTP assíncrono na porta 80.
 *        Se API_AUTH_TOKEN estiver definido em config.h, endpoints da API
 *        exigem autenticação via header X-Api-Token ou query ?token=.
 *        O portal /wifi-setup usa login HTTP Basic com OTA_USERNAME/OTA_PASSWORD.
 *
 * Endpoints disponíveis:
 *   GET /status           — retorna JSON completo do estado do sistema
 *   GET /toggle           — alterna luminária (não afeta a automação por horário)
 *   GET /temperature      — retorna leitura de temperatura + flags available/valid
 *   GET /fan_toggle       — alterna ventoinha (congela modo auto até próximo RTC_ON)
 *   GET /fan_speed?value= — define velocidade da ventoinha 0–100 (congela até RTC_ON)
 *   GET /wifi-setup       — formulário de configuração Wi-Fi (portal de recuperação)
 *   POST /wifi-setup/save — salva SSID/senha e inicia reconexão não-bloqueante
 *   GET /update           — interface web do ElegantOTA (OTA firmware)
 */
void webserver_init();

/**
 * @brief Manutenção do ElegantOTA. Deve ser chamado no loop().
 */
void webserver_loop();
