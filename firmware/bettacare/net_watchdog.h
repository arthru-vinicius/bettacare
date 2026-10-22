#pragma once

#include <stdint.h>

/**
 * Vigia de rede: o que fazer quando o aquário não consegue falar com o servidor.
 *
 * Nasceu do apagão de 21 h de 2026-10-19/20: o Wi-Fi tentou se associar 10.306
 * vezes (motivos de autenticação, nunca de sinal), religou o rádio de 10 em 10
 * minutos, abriu e fechou o AP de recuperação — e nada resolveu até alguém
 * mexer, quase um dia depois. Dois recursos, do mais leve ao mais pesado:
 *
 * 1. **Reassociar** quando o Wi-Fi está de pé mas o servidor não responde há
 *    2 min. Nos 93 períodos sem contato de oito dias, a maioria tinha o Wi-Fi
 *    associado o tempo todo (nenhuma queda contada) e os POSTs expirando — a
 *    cara de um estado travado no ponto de acesso ou na pilha IP, que
 *    desassociar e reassociar limpa (tabela ARP, lease de DHCP, sockets).
 *
 * 2. **Reiniciar** depois de horas sem contato nenhum. O último recurso: não
 *    há mais o que o firmware tentar sozinho. Só com a luminária **apagada**,
 *    porque reiniciar desliga e liga o relé, e o controlador da luminária
 *    troca de padrão (branca/azul/verde/vermelha) a cada liga-e-desliga
 *    rápido. O prazo dobra a cada reinício seguido sem recuperação (2 h, 4 h…
 *    até 24 h): um roteador desligado por um dia não vira um reinício por hora.
 *
 * A decisão é lógica pura, aqui no cabeçalho, para o teste de host
 * (`firmware/test/test_net_watchdog.cpp`) exercitar o código de produção.
 */

/** Wi-Fi associado e POSTs falhando por tanto tempo: reassocia. */
static const uint32_t NET_STALL_PRAZO_MS = 2UL * 60UL * 1000UL;
/** Entre duas reassociações seguidas, ao menos isto — não martelar o ponto de acesso. */
static const uint32_t NET_STALL_ESPERA_MS = 5UL * 60UL * 1000UL;

/** Sem nenhum contato por isto (o primeiro reinício) e a luz apagada: reinicia. */
static const uint32_t NET_WD_PRAZO_INICIAL_MS = 2UL * 60UL * 60UL * 1000UL;
static const uint32_t NET_WD_PRAZO_TETO_MS = 24UL * 60UL * 60UL * 1000UL;
/** Um boot recém-feito ainda não tem "horas sem contato": espera isto de uptime. */
static const uint32_t NET_WD_UPTIME_MINIMO_MS = 30UL * 60UL * 1000UL;
/** Com contato firme por isto depois de um reinício, a sequência volta a zero. */
static const uint32_t NET_WD_RECUPERACAO_MS = 30UL * 60UL * 1000UL;

/**
 * Reassocia? `falhando_ms`: há quanto tempo os POSTs falham com o Wi-Fi de pé.
 * `desde_ultima_ms`: há quanto tempo foi a última reassociação (use o máximo
 * de `uint32_t` se nunca houve).
 */
inline bool net_stall_deve_reassociar(uint32_t falhando_ms, uint32_t desde_ultima_ms) {
  return falhando_ms >= NET_STALL_PRAZO_MS && desde_ultima_ms >= NET_STALL_ESPERA_MS;
}

/** Silêncio exigido para o reinício número `sequencia` (0 = o primeiro): 2 h, 4 h, 8 h… até 24 h. */
inline uint32_t net_watchdog_prazo_ms(uint8_t sequencia) {
  uint32_t prazo = NET_WD_PRAZO_INICIAL_MS;
  for (uint8_t i = 0; i < sequencia && prazo < NET_WD_PRAZO_TETO_MS; i++) prazo *= 2;
  return prazo > NET_WD_PRAZO_TETO_MS ? NET_WD_PRAZO_TETO_MS : prazo;
}

/**
 * Reinicia? `sem_contato_ms`: desde o último POST aceito (ou desde o boot, se
 * nenhum foi). Nunca com a luminária acesa — ver o comentário do arquivo.
 */
inline bool net_watchdog_deve_reiniciar(uint32_t sem_contato_ms, uint8_t sequencia,
                                        bool luz_acesa, uint32_t uptime_ms) {
  if (luz_acesa) return false;
  if (uptime_ms < NET_WD_UPTIME_MINIMO_MS) return false;
  return sem_contato_ms >= net_watchdog_prazo_ms(sequencia);
}

// ── Estado entre reinícios (NVS) ────────────────────────────────────────────

/** Lê a sequência e se este boot veio de um reinício do vigia. */
void net_watchdog_init();

uint8_t net_watchdog_sequencia();

/** Grava a sequência e o silêncio que motivou e reinicia. Não volta. */
void net_watchdog_reiniciar(uint32_t sem_contato_ms);

/**
 * A cada POST aceito: conta o reinício do vigia (um evento só) e, passados
 * `NET_WD_RECUPERACAO_MS` de uptime com o servidor respondendo, zera a sequência.
 */
void net_watchdog_contato_ok(uint32_t uptime_ms);
