#pragma once
#include <Arduino.h>

/**
 * @brief Conecta ao Wi-Fi com as credenciais e IP estático definidos em config.h.
 *        Inicia tentativa de conexão de forma não-bloqueante.
 */
void wifi_connect();

/**
 * @brief Verifica se o Wi-Fi ainda está conectado. Deve ser chamado no loop().
 *        Reconecta automaticamente sem bloquear as demais funcionalidades.
 */
void wifi_check_reconnect();

/**
 * @brief Retorna true se o ESP32 estiver conectado ao Wi-Fi.
 */
bool wifi_is_connected();

/**
 * @brief Informa se o modo de recuperação (AP de configuração) está ativo.
 */
bool wifi_recovery_ap_active();

/**
 * @brief Retorna o SSID do Access Point de recuperação.
 */
String wifi_recovery_ap_ssid();

/**
 * @brief Retorna o SSID atualmente configurado para conexão STA.
 */
String wifi_configured_ssid();

/**
 * @brief Atualiza e persiste credenciais Wi-Fi (NVS), iniciando nova tentativa.
 * @param ssid SSID da rede (1..32 chars)
 * @param password senha da rede (0 ou 8..63 chars)
 * @return true se credenciais válidas e aceitas; false em caso de validação inválida
 */
bool wifi_set_credentials(const String &ssid, const String &password);

/**
 * @brief Quedas de conexão desde o boot — só as de um link que estava de pé,
 *        não as tentativas de reconectar que não fecharam. Subindo depressa,
 *        o link está instável.
 */
uint16_t wifi_reconnect_count();

/**
 * @brief Motivo da última queda (`wifi_err_reason_t` do ESP-IDF): 200 é beacon
 *        perdido, 201 é ponto de acesso não encontrado, 15 é handshake que não
 *        fechou. 0 enquanto não houve queda desde o boot.
 */
uint8_t wifi_last_disconnect_reason();

/** @brief RSSI atual em dBm. Só é medição real quando `wifi_rssi_valid()` também é verdadeiro. */
int16_t wifi_rssi();

/**
 * @brief Falso quando não há associação — o -120 de `wifi_rssi()` é um valor
 *        de repouso, não uma leitura. Sem isto, um snapshot capturado durante
 *        uma queda breve de Wi-Fi podia viajar como se fosse RSSI real, e o
 *        servidor classificaria o link como "fraco" quando na verdade não
 *        havia leitura nenhuma (UPGRADE/03, F13).
 */
bool wifi_rssi_valid();

/** @brief IP local, ou string vazia se desconectado. */
String wifi_local_ip();

// ── Ponto de acesso ─────────────────────────────────────────────────────────
//
// Roteador e repetidor anunciam o mesmo nome de rede; o que os distingue é o
// BSSID (o MAC do rádio de cada um). Até a 2.1.0 o ESP32 entrava no primeiro
// que a varredura encontrasse, e nunca saía dele enquanto a conexão durasse —
// nem quando o outro estava muito mais forte (2026-10-14).

/** Tamanho de um BSSID formatado, "aa:bb:cc:dd:ee:ff" mais o terminador. */
static const uint8_t WIFI_BSSID_STR_LEN = 18;

/** BSSID do ponto de acesso atual, em minúsculas. Falso (e `out` vazio) se desconectado. */
bool wifi_current_bssid(char out[WIFI_BSSID_STR_LEN]);

/** Canal atual; 0 se desconectado. */
uint8_t wifi_current_channel();

/**
 * Fixa o ponto de acesso (persistido na NVS). String vazia ou "auto" volta à
 * escolha automática, pelo sinal mais forte. A troca é aplicada pela task de
 * rede no ciclo seguinte. Falso se o BSSID for inválido.
 *
 * Fixado e sem conseguir conectar por 5 min, o ESP32 usa o mais forte para não
 * ficar sem rede, e volta ao fixado quando uma varredura o encontrar de novo.
 */
bool wifi_set_pinned_bssid(const String &bssid);

/** BSSID fixado. Falso (e `out` vazio) quando a escolha é automática. */
bool wifi_pinned_bssid(char out[WIFI_BSSID_STR_LEN]);

/** Verdadeiro enquanto o fixado não responde e o ESP32 usa o mais forte. */
bool wifi_pin_fallback_active();

// ── Varredura, para o portal /wifi-setup ────────────────────────────────────

struct WifiApInfo {
  char    ssid[33];
  char    bssid[WIFI_BSSID_STR_LEN];
  int16_t rssi;
  uint8_t channel;
  /** Mesmo nome da rede configurada — só estes podem ser fixados. */
  bool    same_ssid;
};

static const uint8_t WIFI_SCAN_MAX = 12;

/**
 * Pede uma varredura. Ela roda na task de rede; sem associação, a reconexão
 * fica pausada durante ela (o ESP-IDF recusa varrer enquanto "conectando").
 */
void wifi_request_scan();

/** Verdadeiro entre o pedido e o fim da varredura. */
bool wifi_scan_busy();

/**
 * Copia o resultado da última varredura (até `max`, do sinal mais forte para
 * o mais fraco). `age_ms` recebe há quanto tempo ela terminou, ou
 * `UINT32_MAX` se nunca houve uma.
 */
uint8_t wifi_scan_results(WifiApInfo *out, uint8_t max, uint32_t *age_ms);
