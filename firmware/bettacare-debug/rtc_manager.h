#pragma once

#include <Arduino.h>

/**
 * DS3231SN e a automação por horário.
 *
 * A regra central é sutil e vem intacta do sistema antigo: **a automação só
 * age na transição de período**. Dentro de uma mesma janela, um override
 * manual (botão, comando do servidor) é respeitado até a próxima mudança de
 * horário. Sem isso, apagar a luz às 14h faria a automação reacendê-la no
 * segundo seguinte.
 *
 * O que muda: os horários vêm de `device_config` — do servidor — em vez de
 * viverem no NVS deste módulo.
 */
void rtc_init();

/**
 * Sincroniza o DS3231 por NTP. **Bloqueia por alguns segundos**, então só pode
 * ser chamado da task de rede.
 */
bool rtc_sync_ntp();

/** Hora local "HH:MM", ou "--:--" se o módulo não responde. */
String rtc_get_time_str();

bool rtc_available();

/** O DS3231 acusou perda de energia — bateria no fim. */
bool rtc_lost_power();

/** Sonda o DS3231 no I²C agora, pelo mutex do barramento. Para o autodiagnóstico. */
bool rtc_probe_present();

/** Avalia a janela horária e age só na transição. Chamado no loop. */
void rtc_check_automation();

/**
 * Reavalia imediatamente após o servidor mandar horários novos, para a
 * mudança valer sem esperar a próxima virada.
 */
void rtc_reapply_schedule();
