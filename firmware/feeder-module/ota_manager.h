#pragma once

/**
 * @brief Sobe o servidor HTTP local e a interface de atualização OTA
 *        (ElegantOTA) em http://<IP_DO_MODULO>/update.
 */
void ota_manager_init();

/** @brief Deve ser chamado a cada iteração do loop(). */
void ota_manager_loop();
