#pragma once
#include <stdint.h>

#include "timeutil.h"

/**
 * A hora do módulo: o DS3231 é o relógio de verdade (bateria, deriva mínima),
 * lido direto nos registradores e **conferido** — transação bem-sucedida e BCD
 * válido — antes de ser aceito. É a mesma lição do principal, onde o RTClib
 * decodificava como hora o lixo de uma leitura falha ("40:08").
 *
 * Entre leituras, a hora anda pelo `millis()`. Com Wi-Fi, o NTP acerta o
 * DS3231 quando os dois discordam por mais de 2 s.
 *
 * Sem hora válida — bateria do DS3231 acabou e nenhum NTP —, a agenda não roda:
 * melhor não alimentar na hora errada.
 */
void clock_init();     // depois do Wire.begin()
void clock_update();   // a cada volta do loop
bool clock_valid();
uint32_t clock_now();  // epoch local; só vale com clock_valid()
bool clock_set(const CivilTime& t);   // acerto pelo console
bool clock_rtc_present();
bool clock_ntp_synced();
