#pragma once

/**
 * Servidor HTTP local do próprio ESP32, na porta 80.
 *
 * Continua existindo por dois motivos que o servidor BettaCare não cobre:
 *
 *   - **OTA** (`/update`) é a única via de atualização de firmware.
 *   - **Recuperação de Wi-Fi** (`/wifi-setup`) precisa funcionar justamente
 *     quando não há rede — e portanto quando o servidor é inalcançável.
 *
 * O que saiu: as rotas que *mudavam* estado (`/toggle`, alteração de horários
 * e limiares). Elas competiriam com o servidor pela mesma decisão, e agora o
 * servidor é a fonte de verdade. O que resta é diagnóstico e recuperação.
 *
 * Roda na task de rede, nunca no `loop()`.
 */
void webserver_init();

/** Manutenção do ElegantOTA. Chamado pela task de rede. */
void webserver_loop();

/**
 * Desfecho da última atualização OTA: `"none"`, `"ok"` ou `"failed"`.
 *
 * **Persistido em NVS**, e essa é a parte que importa: um OTA bem-sucedido
 * reinicia o dispositivo imediatamente, então um resultado guardado só em RAM
 * se perderia justamente no caso bom. Guardando na flash, o `diag` do primeiro
 * POST depois do reboot consegue contar o que houve.
 *
 * Antes disto, `ota` era um componente declarado no contrato que nunca emitia
 * nada: os callbacks só imprimiam no Serial, e uma atualização que falhava era
 * invisível para o servidor — num sistema onde OTA é a via de recuperação.
 */
const char* webserver_ota_last_result();
