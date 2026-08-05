#pragma once

#include <Arduino.h>

/**
 * Configuração do aquário.
 *
 * A inversão mais importante desta reescrita: **o servidor é a fonte de
 * verdade e o NVS virou cache offline**. Antes os horários viviam no
 * dispositivo e a interface os alterava; agora o dispositivo compara o
 * `config_version` a cada POST e aplica quando diverge.
 *
 * O que não muda: ele continua operando sozinho sem rede, com a última
 * configuração conhecida. É por isso que a cópia em NVS existe — sem ela, um
 * ESP32 reiniciado com o servidor fora do ar não saberia a que horas acender a
 * luz.
 *
 * Thread-safety: **escrito pela task de rede, lido pelo controle local**, e a
 * struct tem 24 bytes — ou seja, mais de uma palavra. Sem proteção, o loop
 * poderia ler metade da configuração nova e metade da velha, e uma combinação
 * rasgada em que `fan_off_c >= fan_trigger_c` faria a ventoinha oscilar. Por
 * isso a leitura é **por cópia sob mutex**, e não por referência.
 */

struct DeviceConfig {
  uint8_t  light_on_hour;
  uint8_t  light_on_min;
  uint8_t  light_off_hour;
  uint8_t  light_off_min;
  float    fan_trigger_c;
  float    fan_off_c;
  uint32_t telemetry_interval_ms;
  uint32_t heartbeat_interval_ms;
};

/** Carrega do NVS; cai nos padrões de `config.h` se não houver nada válido. */
void device_config_init();

/**
 * Cópia consistente da configuração. É a forma correta de lê-la de qualquer
 * núcleo — nunca guarde a referência entre ciclos.
 */
DeviceConfig device_config_snapshot();

/** Versão que o dispositivo tem agora. Zero significa "nunca falei com o servidor". */
uint32_t device_config_version();

/**
 * Aplica a configuração recebida do servidor e persiste no NVS.
 *
 * Valida antes de gravar: horário fora de faixa ou histerese invertida
 * (`fan_off_c >= fan_trigger_c`) faria a ventoinha oscilar sem parar. Nesse
 * caso a configuração é recusada, a anterior é mantida e um evento de erro é
 * registrado — melhor operar com a regra velha do que com uma impossível.
 *
 * @return false se a configuração foi recusada.
 */
bool device_config_apply(const DeviceConfig& cfg, uint32_t version);

/** Horários formatados "HH:MM", para exibição e log. */
String device_config_on_time();
String device_config_off_time();
