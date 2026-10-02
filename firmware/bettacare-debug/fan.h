#pragma once

#include <Arduino.h>

#include "app_state.h"

/**
 * Ventoinha de 4 pinos com tacômetro.
 *
 * A máquina de estados (histerese, cooldown de 30 min, escalonamento
 * progressivo, failsafe de temperatura indisponível e calibração do
 * potenciômetro) é portada **quase literalmente** do sistema antigo. É a
 * lógica mais madura do projeto e não tem nada a ver com o transporte.
 *
 * Duas coisas mudam:
 *
 * 1. Os limiares vêm de `device_config` — ou seja, do servidor — em vez de
 *    viverem no NVS deste módulo.
 * 2. O tacômetro passa a ser vigiado: PWM acima de zero com rotação zerada é a
 *    **única realimentação real do sistema inteiro**, e significa fisicamente
 *    uma coisa só — a ventoinha não está girando.
 */
void fan_init();

/** Controle completo. Chamado no loop, depois de `temperature_update()`. */
void fan_update();

/**
 * Volta para AUTO e exige re-calibração do potenciômetro.
 *
 * Chamado na transição para o período ON da luminária. A re-calibração evita
 * que a posição residual do pot reative a ventoinha sozinha.
 */
void fan_on_schedule_reset();

/** Velocidade fixa por comando do servidor. `percent == 0` desliga. */
void fan_set_speed(int percent);

/** Alterna entre controle automático e manual, por comando do servidor. */
void fan_set_mode_auto(bool automatico);

int  fan_get_speed_percent();
bool fan_is_on();
int  fan_get_rpm();

/** Leitura filtrada do ADC do potenciômetro — para o bloco `diag` da telemetria. */
int fan_get_pot_raw_adc();

/** Pulsos crus contados na última janela de 2 s, antes de virar RPM. */
uint32_t fan_get_tach_pulses_raw();

/** Modo como ele é reportado ao servidor. */
FanModeReport fan_get_mode_report();
