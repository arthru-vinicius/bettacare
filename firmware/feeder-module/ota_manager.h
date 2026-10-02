#pragma once

/**
 * @brief Sobe o servidor HTTP local e a interface de atualização OTA
 *        (ElegantOTA) em http://<IP_DO_MODULO>/update.
 */
void ota_manager_init();

/** @brief Deve ser chamado a cada iteração do loop(). */
void ota_manager_loop();

/**
 * @brief Uma atualização está sendo gravada: nenhuma refeição começa, e a que
 *        estiver em andamento é interrompida em estado seguro — o módulo
 *        reinicia no fim.
 */
bool ota_in_progress();
