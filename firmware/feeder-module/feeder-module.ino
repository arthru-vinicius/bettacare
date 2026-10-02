#include <Arduino.h>
#include <Wire.h>

#include "app.h"
#include "clock.h"
#include "config.h"
#include "debuglog.h"
#include "console.h"
#include "doser.h"
#include "hw.h"
#include "link.h"
#include "ota_manager.h"
#include "store.h"
#include "ui.h"
#include "wifi_manager.h"

// Firmware do módulo alimentador (ESP32-C3 Super Mini). O mapa dos arquivos
// e a bancada estão no README.md desta pasta; montagem e pinos em
// docs/pinagem-alimentador-modulo.md.
//
// O loop nunca espera: o doseador é uma máquina de estados, o botão é
// amostrado por timer e o enlace lê o que já chegou no UART. Quem bloqueia
// é só o desenho da tela (~25 ms a 400 kHz), e só com ela acesa.

void setup() {
  // Motores, LED IR e servo desligados antes de qualquer outra coisa — os
  // pull-downs da placa só seguram até aqui.
  hw_init();

  Serial.begin(115200);
  log_init();   // daqui em diante o log também fica na RAM e vai pela rede
  Log.printf("[Boot] %s fw=%s, reinicio: %s\n", DEVICE_ID, FW_VERSION, log_reset_reason());

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  store_init();
  clock_init();
  link_init();
  ui_init();    // tela e botão
  app_init();   // depois de store, clock e link

  wifi_connect();
  ota_manager_init();

  // Loop travado por 5 s reinicia o módulo: um motor nunca fica ligado
  // esperando um loop que não volta.
  enableLoopWDT();
  Log.println("[Boot] Pronto. 'ajuda' lista os comandos do console.");
}

void loop() {
  wifi_check_reconnect();
  ota_manager_loop();
  clock_update();
  app_update();
  doser_update();
  ui_update();
  console_update();
  delay(1);   // a tarefa ociosa e o Wi-Fi respiram; o resto tem folga de sobra
}
