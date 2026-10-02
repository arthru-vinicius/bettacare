#pragma once

/**
 * Console da bancada, pela USB (115200 baud, um comando por linha): estado,
 * hora, agenda, calibração e um teste para cada peça. `ajuda` lista tudo.
 *
 * Os testes não são refeição — não entram no limite de 24 h nem no histórico —,
 * e `alimentar` passa pelas mesmas regras do botão.
 */
void console_update();   // a cada volta do loop

/**
 * Um comando vindo da rede (`POST /console`): roda na próxima volta do loop, e
 * a saída vai para o log (`GET /log`, UDP). Falso se o anterior ainda não
 * rodou, ou se a linha for vazia ou longa demais.
 */
bool console_enqueue(const char* line);
