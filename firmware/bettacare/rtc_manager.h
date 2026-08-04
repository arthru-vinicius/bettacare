#pragma once
#include <Arduino.h>

/**
 * @brief Inicializa a comunicação I2C com o DS3231SN via RTClib.
 *        Detecta presença do módulo e imprime a hora atual no Serial.
 */
void rtc_init();

/**
 * @brief Sincroniza o DS3231SN com horário NTP (Brasília, UTC-3).
 *        Deve ser chamado após wifi_connect(). Se o NTP falhar,
 *        mantém a hora já armazenada no módulo.
 * @return true quando sincronizado com sucesso; false se indisponível/falhou.
 */
bool rtc_sync_ntp();

/**
 * @brief Retorna a hora atual como string no formato "HH:MM".
 * @return hora formatada, ou "--:--" se o RTC não estiver disponível
 */
String rtc_get_time_str();

/**
 * @brief Retorna true se o DS3231SN foi encontrado durante a inicialização.
 */
bool rtc_available();

/**
 * @brief Verifica transições de horário e comanda a luminária conforme o
 *        schedule definido em config.h (RTC_ON_* / RTC_OFF_*).
 *
 *        A automação é sempre ativa. O botão físico e o web apenas sobrepõem
 *        o estado dentro do período atual; na próxima transição a luminária
 *        voltará a seguir o schedule normalmente.
 *        Deve ser chamado no loop().
 */
void rtc_check_automation();

/**
 * @brief Retorna o horário de ligar configurado como "HH:MM".
 */
String rtc_get_on_time();

/**
 * @brief Retorna o horário de desligar configurado como "HH:MM".
 */
String rtc_get_off_time();

/**
 * @brief Atualiza horário de ligar/desligar da automação e persiste em NVS.
 *        A nova programação é aplicada imediatamente.
 * @return true se os valores forem válidos e aplicados.
 */
bool rtc_set_schedule(uint8_t on_hour, uint8_t on_min, uint8_t off_hour, uint8_t off_min);
