#pragma once
#include "protocol.h"

/**
 * O fio com o ESP32 principal: 9600 baud no GPIO20 (RX) e no GPIO21 (TX), com
 * 4,7 kΩ e 1 kΩ em série na placa. São os pinos da UART0, mas quem os usa é a
 * UART1 — a UART0 é o console do sistema, e log nenhum pode cair no meio de
 * uma linha do protocolo. A UART em repouso deixa o TX em nível alto — é por
 * ele que o principal detecta o módulo no fio (firmware 2.0.1 de lá).
 *
 * O principal pergunta (`PING`) a cada 2 s. Avisos que saem com ele fora do
 * fio — resultado de refeição, recusa, refeição perdida — esperam numa fila
 * curta e vão quando ele voltar.
 */
void link_init();

/** Lê o UART; verdadeiro quando chega um pedido completo e válido. */
bool link_poll(LinkRequest& req);

/** O principal mandou `PING` nos últimos 6 s. */
bool link_connected();

/** Verdadeiro uma vez, no primeiro `PING` depois de um silêncio. */
bool link_take_reconnected();

/** Manda já (resposta a um pedido). */
void link_send(const char* line);

/** Aviso: manda já se o principal está no fio; senão, guarda para a volta. */
void link_notify(const char* line);

/** Esvazia a fila de avisos — chamado na reconexão. */
void link_flush_pending();
