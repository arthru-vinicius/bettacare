#pragma once

#include <Arduino.h>

/**
 * A task de rede, fixada no núcleo 0.
 *
 * Tudo que pode bloquear vive aqui: associação Wi-Fi, DNS, conexão TCP,
 * requisição HTTP, sincronização NTP e manutenção do OTA. O `loop()` no núcleo
 * 1 nunca espera por nenhuma dessas coisas.
 *
 * Essa separação é a correção de um defeito real do firmware antigo, não um
 * refinamento: lá o loop era single-threaded e o handshake TLS do MQTT
 * bloqueava até 6 segundos, então uma instabilidade de rede travava o botão
 * físico e a histerese da ventoinha ao mesmo tempo.
 */
void net_task_start();

/** Verdadeiro depois do primeiro POST aceito pelo servidor. */
bool net_task_ever_connected();
