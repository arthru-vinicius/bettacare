#pragma once

#include <Arduino.h>

/**
 * O cliente do servidor BettaCare.
 *
 * Um único `POST /api/v1/telemetry` serve os dois sentidos: o corpo leva o
 * estado do aquário, a **resposta traz os comandos pendentes**. O servidor
 * nunca inicia conexão, e é por isso que o IP dinâmico do ESP32 deixou de ser
 * um problema — ninguém precisa alcançá-lo.
 *
 * Roda inteiramente na task de rede (núcleo 0). Nada aqui pode ser chamado do
 * `loop()`.
 */

enum ApiResult : uint8_t {
  API_OK = 0,
  API_NO_WIFI,
  API_HTTP_ERROR,     // não houve resposta, ou o código não foi 2xx
  API_UNAUTHORIZED,   // 401 — token divergente
  API_SERVER_ERROR,   // 5xx — o servidor está de pé mas não pôde atender
  API_BAD_RESPONSE,   // 2xx com corpo que não dá para interpretar
};

void api_client_init();

/**
 * Monta e envia um POST, e processa a resposta.
 *
 * Em caso de sucesso: aplica a configuração se o `config_version` divergir e
 * enfileira os comandos recebidos para o controle local executar.
 *
 * O `seq` **só avança quando a troca é bem-sucedida**. Se a resposta se perder,
 * a próxima tentativa repete o mesmo `seq`, o servidor reconhece o reenvio e
 * devolve a mesma lista de comandos — que é exatamente o que o contrato pede.
 */
ApiResult api_client_post();

/** Falhas consecutivas desde o último POST bem-sucedido. */
uint16_t api_client_consecutive_failures();

/** `millis()` do último POST aceito, ou 0 se nunca houve um. */
uint32_t api_client_last_success_ms();

/**
 * Código HTTP da última resposta: 200 é aceito, 400 é corpo recusado, negativo
 * é erro de conexão do HTTPClient, 0 é "nenhum POST ainda".
 */
int api_client_last_http_status();
