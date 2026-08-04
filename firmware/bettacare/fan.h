#pragma once

/**
 * @brief Inicializa o canal LEDC (PWM) e o tacômetro da ventoinha.
 *        Estado inicial: desligada. Modo inicial: AUTO.
 *        Implementação: fan 4 pinos (CPU fan) com PWM direto no PIN_FAN
 *        e leitura de RPM via interrupção no PIN_FAN_TACH.
 */
void fan_init();

/**
 * @brief Gerencia toda a lógica de controle da ventoinha.
 *        Lê o potenciômetro, monitora a temperatura (via cache do DS18B20) e
 *        executa a máquina de estados do controle automático com histérese.
 *        Se a temperatura ficar indisponível/stale no modo AUTO, aplica
 *        FAN_FAILSAFE_SPEED como fallback de segurança.
 *        Deve ser chamado no loop(), após temperature_update().
 */
void fan_update();

/**
 * @brief Reseta a ventoinha para o modo AUTO e requer re-calibração do
 *        potenciômetro. Chamado pelo RTC na transição para o período ON
 *        (RTC_ON_HOUR:RTC_ON_MIN), sincronizado com o acendimento da luminária.
 */
void fan_on_rtc_reset();

/**
 * @brief Define a velocidade manualmente via interface web.
 *        Congela o controle automático até o próximo RTC_ON.
 *        Se percent == 0, equivale a fan_toggle_web() no estado ligado.
 * @param percent velocidade desejada (0–100)
 */
void fan_set_speed_web(int percent);

/**
 * @brief Alterna o estado da ventoinha via interface web.
 *        Se desligada: liga na última velocidade manual usada (padrão: 50%).
 *        Se ligada: desliga em modo MANUAL_OFF e reseta calibração do pot.
 *        Congela o controle automático até o próximo RTC_ON.
 */
void fan_toggle_web();

/**
 * @brief Retorna a velocidade atual da ventoinha como percentual (0–100).
 *        Observação: o hardware pode aplicar duty mínimo/boost de partida.
 */
int fan_get_speed_percent();

/**
 * @brief Retorna true se a ventoinha estiver ligada (velocidade > 0).
 */
bool fan_is_on();

/**
 * @brief Retorna a rotação atual da ventoinha em RPM.
 *        Valor calculado a cada 2 segundos a partir do sinal de tacômetro (pino 3 da fan).
 *        Retorna 0 se a ventoinha estiver parada ou durante o primeiro período de amostragem.
 */
int fan_get_rpm();

/**
 * @brief Atualiza os thresholds do modo AUTO e persiste em NVS.
 * @param trigger_c temperatura para ligar a ventoinha
 * @param off_c     temperatura para iniciar desligamento/cooldown
 * @return true se valores válidos e aplicados; false caso inválidos
 */
bool fan_set_auto_thresholds(float trigger_c, float off_c);

/**
 * @brief Retorna o threshold de temperatura para ligar no AUTO.
 */
float fan_get_trigger_c();

/**
 * @brief Retorna o threshold de temperatura para desligar/cooldown no AUTO.
 */
float fan_get_off_c();
