#include "wifi_manager.h"
#include "config.h"
#include "event_log.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <string.h>

// Intervalo entre novas tentativas de conexão (não-bloqueante)
#ifndef WIFI_RECONNECT_INTERVAL_MS
#define WIFI_RECONNECT_INTERVAL_MS 10000UL
#endif

// Ativa AP de recuperação após tempo contínuo sem conectar (ms)
#ifndef WIFI_RECOVERY_TIMEOUT_MS
#define WIFI_RECOVERY_TIMEOUT_MS 180000UL
#endif

// Ativa AP de recuperação após N tentativas sem sucesso
#ifndef WIFI_RECOVERY_MAX_ATTEMPTS
#define WIFI_RECOVERY_MAX_ATTEMPTS 12
#endif

// Prefixo do SSID usado no AP de recuperação
#ifndef WIFI_RECOVERY_AP_SSID_PREFIX
#define WIFI_RECOVERY_AP_SSID_PREFIX "Aquarium-Setup"
#endif

/**
 * Segundos sem receber beacon antes de desassociar. O padrão do ESP-IDF é 6.
 *
 * O RSSI do aquário salta de -84 para -55 e volta em segundos (produção,
 * 2026-10): com 6 s, uma oscilação dessas derrubava a associação inteira, e
 * reassociar com sinal ruim chegou a levar horas. Com 20 s o enlace atravessa
 * a oscilação e quem lida com o pacote perdido é o TCP, que existe para isso.
 */
#ifndef WIFI_BEACON_TIMEOUT_S
#define WIFI_BEACON_TIMEOUT_S 20
#endif

/**
 * Sem Wi-Fi por tanto tempo, o rádio é desligado e religado — de novo a cada
 * intervalo igual, enquanto não voltar. Sai de um estado preso do driver sem
 * reiniciar o ESP32: a luz, a ventoinha e o comando manual seguem intactos.
 */
#ifndef WIFI_RADIO_RESTART_MS
#define WIFI_RADIO_RESTART_MS (10UL * 60UL * 1000UL)
#endif

/** Ponto de acesso fixado sem conectar por tanto tempo: usa o mais forte. */
#ifndef WIFI_PIN_FALLBACK_MS
#define WIFI_PIN_FALLBACK_MS (5UL * 60UL * 1000UL)
#endif

/** No modo "usando o mais forte", de quanto em quanto tempo procurar o fixado. */
#ifndef WIFI_PIN_RETURN_CHECK_MS
#define WIFI_PIN_RETURN_CHECK_MS (30UL * 60UL * 1000UL)
#endif

/** Sinal mínimo do fixado para voltar a ele depois do fallback. */
static const int16_t PIN_RETURN_MIN_RSSI = -85;

/** Uma varredura que não termina nisto é abandonada (e a reconexão retomada). */
static const unsigned long SCAN_TIMEOUT_MS = 15000;

/** Quanto esperar o motivo da queda chegar do callback antes de registrar sem ele. */
static const unsigned long DROP_REASON_WAIT_MS = 1500;

static Preferences _prefs;

/**
 * Protege `_configured_ssid`, `_configured_password`, `_recovery_ap_ssid` e o
 * BSSID pedido pelo portal (UPGRADE/03, F6). São escritos de três contextos —
 * a task de rede (núcleo 0), o `setup()` no boot, e a task do AsyncTCP quando
 * alguém usa o portal de recuperação —, e sem proteção, um deles podia seguir
 * um ponteiro que o outro acabou de liberar.
 */
static SemaphoreHandle_t _cred_mutex = nullptr;

static void _ensure_cred_mutex() {
  if (_cred_mutex == nullptr) _cred_mutex = xSemaphoreCreateMutex();
}

static bool          _prefs_loaded       = false;
static bool          _ip_configured_once = false;
static bool          _ever_connected     = false;
static bool          _recovery_ap_active = false;
static unsigned long _last_attempt_ms    = 0;
static unsigned long _offline_since_ms   = 0;
static uint16_t      _attempt_count      = 0;

static String _configured_ssid;
static String _configured_password;
static String _recovery_ap_ssid;

// ── Ponto de acesso fixado ──────────────────────────────────────────────────
// Só a task de rede lê e escreve estes; o portal pede a troca por `_pin_req*`.
static uint8_t       _pin[6]       = {};
static bool          _has_pin      = false;
static bool          _pin_fallback = false;
static unsigned long _pin_check_ms = 0;
static bool          _pin_check_pending = false;

// Pedido do portal (sob `_cred_mutex`), aplicado no próximo ciclo da rede.
static bool    _pin_req_pending = false;
static bool    _pin_req_has     = false;
static uint8_t _pin_req[6]      = {};

// ── O que o driver conta (task de eventos do Arduino) ──────────────────────
// Escritos só pelo callback; lidos soltos pela task de rede (palavras únicas).
static volatile bool     _link_up        = false;
/** A próxima desassociação foi pedida por nós (troca de AP, varredura). */
static volatile bool     _voluntary      = false;
static volatile uint16_t _drops          = 0;
static volatile uint8_t  _drop_reason    = 0;
/** Tentativas de reconectar que não fecharam, desde a última conexão. */
static volatile uint16_t _attempt_fails  = 0;
static volatile uint8_t  _attempt_reason = 0;

// Só a task de rede.
static int16_t       _rssi_before      = 0;
static uint16_t      _drops_logged     = 0;
static unsigned long _drop_seen_ms     = 0;
static unsigned long _radio_restart_ms = 0;

// ── Varredura ───────────────────────────────────────────────────────────────
static SemaphoreHandle_t _scan_mutex = nullptr;
static WifiApInfo        _scan[WIFI_SCAN_MAX];
static uint8_t           _scan_n = 0;
static unsigned long     _scan_done_ms = 0;
static volatile bool     _scan_requested = false;
static bool              _scan_running = false;
static unsigned long     _scan_started_ms = 0;
/** A varredura começou sem associação: a reconexão volta quando ela terminar. */
static bool              _scan_paused_connect = false;

static void _ensure_scan_mutex() {
  if (_scan_mutex == nullptr) _scan_mutex = xSemaphoreCreateMutex();
}

// ── BSSID ───────────────────────────────────────────────────────────────────

static void _format_bssid(const uint8_t *b, char *out) {
  snprintf(out, WIFI_BSSID_STR_LEN, "%02x:%02x:%02x:%02x:%02x:%02x",
           b[0], b[1], b[2], b[3], b[4], b[5]);
}

static int _hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/** "aa:bb:cc:dd:ee:ff" (ou com '-'), maiúsculas ou minúsculas. */
static bool _parse_bssid(const char *s, uint8_t *out) {
  if (s == nullptr || strlen(s) != 17) return false;
  for (int i = 0; i < 6; i++) {
    int alto = _hex(s[i * 3]);
    int baixo = _hex(s[i * 3 + 1]);
    if (alto < 0 || baixo < 0) return false;
    if (i < 5 && s[i * 3 + 2] != ':' && s[i * 3 + 2] != '-') return false;
    out[i] = (uint8_t)(alto * 16 + baixo);
  }
  return true;
}

// ── Credenciais ─────────────────────────────────────────────────────────────

static bool _is_valid_ssid(const String &ssid) {
  return ssid.length() > 0 && ssid.length() <= 32;
}

static bool _is_valid_password(const String &password) {
  // Senha WPA2: 8..63; rede aberta: 0
  return password.length() == 0 || (password.length() >= 8 && password.length() <= 63);
}

static void _load_credentials_once() {
  if (_prefs_loaded) return;
  _ensure_cred_mutex();

  String ssid = WIFI_SSID;
  String password = WIFI_PASSWORD;
  String bssid;

  if (_prefs.begin("wifi_cfg", true)) {
    String savedSsid = _prefs.getString("ssid", "");
    String savedPass = _prefs.getString("pass", "");
    bssid = _prefs.getString("bssid", "");
    _prefs.end();

    savedSsid.trim();
    if (_is_valid_ssid(savedSsid) && _is_valid_password(savedPass)) {
      ssid     = savedSsid;
      password = savedPass;
      Serial.printf("[WiFi] Credenciais carregadas da NVS (SSID: %s)\n", ssid.c_str());
    } else {
      Serial.println("[WiFi] Sem credencial válida na NVS. Usando config.h");
    }
  } else {
    // Abrir só-leitura um namespace que nunca foi gravado falha por definição —
    // é o caso normal enquanto ninguém salvou credenciais pelo portal, não
    // defeito de NVS (a mensagem antiga dizia "Falha" em todo boot).
    Serial.println("[WiFi] Nenhuma credencial salva pelo portal. Usando config.h");
  }

  // A I/O de NVS acima roda fora do mutex — só a troca do estado compartilhado
  // precisa dele, e mantê-lo preso durante a leitura do flash prenderia quem
  // mais estivesse esperando por muito mais tempo que o necessário.
  xSemaphoreTake(_cred_mutex, portMAX_DELAY);
  _configured_ssid     = ssid;
  _configured_password = password;
  xSemaphoreGive(_cred_mutex);

  if (bssid.length() > 0 && _parse_bssid(bssid.c_str(), _pin)) {
    _has_pin = true;
    Serial.printf("[WiFi] Ponto de acesso fixado: %s\n", bssid.c_str());
  }

  _prefs_loaded = true;
}

static bool _save_credentials(const String &ssid, const String &password) {
  if (!_prefs.begin("wifi_cfg", false)) {
    Serial.println("[WiFi] Falha ao abrir NVS para escrita");
    return false;
  }

  size_t writtenSsid = _prefs.putString("ssid", ssid);
  _prefs.putString("pass", password);
  _prefs.end();

  return writtenSsid > 0;
}

static void _configure_static_ip_once() {
  if (_ip_configured_once) return;
  _ip_configured_once = true;

  // "0.0.0.0" é o sinal para usar DHCP — útil quando a rede muda e o portal
  // de recuperação não tem como alterar o IP estático em runtime.
  IPAddress local_IP;
  if (!local_IP.fromString(NET_LOCAL_IP) || local_IP == IPAddress(0, 0, 0, 0)) {
    Serial.println("[WiFi] IP dinamico (DHCP) — IP atribuido sera exibido ao conectar");
    return;  // Não chama WiFi.config: usa DHCP automaticamente
  }

  IPAddress gateway, subnet;
  gateway.fromString(NET_GATEWAY);
  subnet.fromString(NET_SUBNET);

  if (!WiFi.config(local_IP, gateway, subnet)) {
    Serial.println("[WiFi] Falha ao configurar IP estatico — usando DHCP como fallback");
  } else {
    Serial.printf("[WiFi] IP estatico configurado: %s\n", NET_LOCAL_IP);
  }
}

static String _build_recovery_ap_ssid() {
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%06lX", (unsigned long)(ESP.getEfuseMac() & 0xFFFFFF));

  String ssid = WIFI_RECOVERY_AP_SSID_PREFIX;
  ssid += "-";
  ssid += suffix;
  return ssid;
}

static void _start_recovery_ap(bool anunciar = true) {
  if (_recovery_ap_active) return;

  String ssid = _build_recovery_ap_ssid();
  WiFi.mode(WIFI_AP_STA);

  size_t otaPassLen = strlen(OTA_PASSWORD);
  if (otaPassLen < 8 || otaPassLen > 63) {
    Serial.println("[WiFi] OTA_PASSWORD invalido para WPA2 (use 8..63 chars). AP de recuperacao nao iniciado.");
    return;
  }

  bool started = WiFi.softAP(ssid.c_str(), OTA_PASSWORD);

  if (!started) {
    Serial.println("[WiFi] Falha ao iniciar AP de recuperacao");
    return;
  }

  _ensure_cred_mutex();
  xSemaphoreTake(_cred_mutex, portMAX_DELAY);
  _recovery_ap_ssid = ssid;
  xSemaphoreGive(_cred_mutex);

  _recovery_ap_active = true;
  if (anunciar) {
    event_log(SEV_WARN, COMP_WIFI, "wifi.ap_mode",
              "AP de recuperacao ativo: %s", ssid.c_str());
  }
  Serial.printf("[WiFi] AP de recuperacao ativo: SSID=%s IP=%s\n",
                ssid.c_str(),
                WiFi.softAPIP().toString().c_str());
  Serial.println("[WiFi] Acesse /wifi-setup e autentique com login/senha OTA");
}

static void _stop_recovery_ap() {
  if (!_recovery_ap_active) return;

  WiFi.softAPdisconnect(true);
  // Retorna explicitamente para STA para reduzir superfície de rede e consumo.
  WiFi.mode(WIFI_STA);
  _recovery_ap_active = false;

  _ensure_cred_mutex();
  xSemaphoreTake(_cred_mutex, portMAX_DELAY);
  _recovery_ap_ssid = "";
  xSemaphoreGive(_cred_mutex);

  event_log(SEV_INFO, COMP_WIFI, "wifi.ap_mode_off",
            "AP de recuperacao desativado");
  Serial.println("[WiFi] AP de recuperacao desativado");
}

// ── Rádio ───────────────────────────────────────────────────────────────────

/**
 * Ajustes para um enlace ruim, reaplicados a cada tentativa — o driver perde
 * parte deles ao trocar de modo (STA ↔ AP+STA) ou ao ser religado.
 */
static void _tune_radio() {
  // Sem economia de energia: com ela o rádio dorme entre beacons e, com sinal
  // fraco, perder alguns basta para o AP ser dado como sumido. O ESP32 é
  // alimentado pela fonte do aquário — a economia não serve para nada aqui.
  WiFi.setSleep(false);
  // Varre todos os canais e escolhe o mais forte. O padrão (`WIFI_FAST_SCAN`)
  // entra no primeiro AP com o nome certo — roteador ou repetidor, o que
  // aparecer antes na ordem dos canais, por mais fraco que esteja.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  // 20 MHz: metade da banda, mais alcance e menos sensibilidade a vizinhos.
  // O tráfego daqui é de centenas de bytes por segundo.
  esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
  esp_wifi_set_inactive_time(WIFI_IF_STA, WIFI_BEACON_TIMEOUT_S);
}

static void _start_connect_attempt(const char* reason) {
  _load_credentials_once();
  _configure_static_ip_once();

  // Cópia local antes de `WiFi.begin()`: a biblioteca copia os bytes na hora
  // da chamada, então segurar o mutex além deste ponto não teria efeito.
  String ssid, password;
  xSemaphoreTake(_cred_mutex, portMAX_DELAY);
  ssid     = _configured_ssid;
  password = _configured_password;
  xSemaphoreGive(_cred_mutex);

  if (!_is_valid_ssid(ssid)) {
    // Evita loop de tentativas em alta frequência quando SSID está inválido.
    _last_attempt_ms = millis();
    Serial.println("[WiFi] SSID invalido. Aguardando nova configuracao.");
    return;
  }

  WiFi.mode(_recovery_ap_active ? WIFI_AP_STA : WIFI_STA);
  _tune_radio();

  bool fixo = _has_pin && !_pin_fallback;
  if (fixo) {
    WiFi.begin(ssid.c_str(), password.c_str(), 0, _pin);
  } else {
    WiFi.begin(ssid.c_str(), password.c_str());
  }

  _last_attempt_ms = millis();
  if (_offline_since_ms == 0) _offline_since_ms = _last_attempt_ms;
  _attempt_count++;

  Serial.printf("[WiFi] Tentativa #%u (%s) SSID=%s%s\n",
                (unsigned)_attempt_count,
                reason,
                ssid.c_str(),
                fixo ? " (AP fixado)" : "");
}

/**
 * Desassocia e conecta de novo, com a configuração atual (AP fixado ou não).
 * A desassociação pedida não conta como queda, e impede o religamento
 * automático do core de insistir na configuração antiga.
 */
static void _restart_connection(const char* reason) {
  if (_link_up) _voluntary = true;
  // Pedida, não perdida: sem `wifi.disconnected`. A volta registra o
  // `wifi.connected` com o AP novo, que é o que interessa.
  _ever_connected = false;
  WiFi.disconnect(false, false);
  _start_connect_attempt(reason);
}

/** Desliga e religa o rádio inteiro — ver `WIFI_RADIO_RESTART_MS`. */
static void _restart_radio() {
  bool ap = _recovery_ap_active;
  if (_link_up) _voluntary = true;
  _ever_connected = false;
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  delay(200);

  // O AP de recuperação e o IP estático vão junto com o rádio.
  _recovery_ap_active = false;
  _ip_configured_once = false;
  if (ap) _start_recovery_ap(false);
  _start_connect_attempt("radio-restart");
}

static void _on_wifi_event(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    _link_up = true;
    _voluntary = false;
    return;
  }
  if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) return;

  uint8_t motivo = (uint8_t)info.wifi_sta_disconnected.reason;
  if (_voluntary) {
    _voluntary = false;
    _link_up = false;
    return;
  }
  if (_link_up) {
    _link_up = false;
    if (_drops < 65535) _drops = _drops + 1;
    _drop_reason = motivo;
  } else {
    if (_attempt_fails < 65535) _attempt_fails = _attempt_fails + 1;
    _attempt_reason = motivo;
  }
}

// ── Varredura ───────────────────────────────────────────────────────────────

static void _maybe_return_to_pin();

static void _resume_after_scan() {
  if (!_scan_paused_connect) return;
  _scan_paused_connect = false;
  _start_connect_attempt("after-scan");
}

static void _store_scan(int16_t n) {
  String configurado;
  xSemaphoreTake(_cred_mutex, portMAX_DELAY);
  configurado = _configured_ssid;
  xSemaphoreGive(_cred_mutex);

  WifiApInfo tmp[WIFI_SCAN_MAX];
  uint8_t usados = 0;
  for (int16_t i = 0; i < n; i++) {
    WifiApInfo ap = {};
    String ssid = WiFi.SSID(i);
    strncpy(ap.ssid, ssid.c_str(), sizeof(ap.ssid) - 1);
    const uint8_t *b = WiFi.BSSID(i);
    if (b == nullptr) continue;
    _format_bssid(b, ap.bssid);
    ap.rssi = (int16_t)WiFi.RSSI(i);
    ap.channel = (uint8_t)WiFi.channel(i);
    ap.same_ssid = ssid == configurado;

    // Mantém os mais fortes: com a lista cheia, entra no lugar do mais fraco.
    if (usados < WIFI_SCAN_MAX) {
      tmp[usados++] = ap;
    } else {
      uint8_t pior = 0;
      for (uint8_t k = 1; k < usados; k++) {
        if (tmp[k].rssi < tmp[pior].rssi) pior = k;
      }
      if (ap.rssi > tmp[pior].rssi) tmp[pior] = ap;
    }
  }
  // Do mais forte para o mais fraco (são no máximo 12: inserção basta).
  for (uint8_t i = 1; i < usados; i++) {
    WifiApInfo x = tmp[i];
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && tmp[j].rssi < x.rssi) {
      tmp[j + 1] = tmp[j];
      j--;
    }
    tmp[j + 1] = x;
  }

  _ensure_scan_mutex();
  xSemaphoreTake(_scan_mutex, portMAX_DELAY);
  memcpy(_scan, tmp, sizeof(WifiApInfo) * usados);
  _scan_n = usados;
  _scan_done_ms = millis();
  xSemaphoreGive(_scan_mutex);
}

static void _poll_scan(unsigned long now) {
  if (!_scan_running) {
    if (!_scan_requested) return;
    _scan_requested = false;

    // Sem associação, a estação está sempre "conectando" (o core religa a
    // cada falha) e o ESP-IDF recusa a varredura — justamente no caso em que
    // o portal é aberto. Pausa a reconexão e a retoma no fim.
    _scan_paused_connect = WiFi.status() != WL_CONNECTED;
    if (_scan_paused_connect) {
      WiFi.disconnect(false, false);
    }
    int16_t r = WiFi.scanNetworks(true, true, false, 150);
    if (r == WIFI_SCAN_FAILED) {
      Serial.println("[WiFi] Varredura recusada pelo driver");
      _resume_after_scan();
      return;
    }
    _scan_running = true;
    _scan_started_ms = now;
    return;
  }

  int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING && now - _scan_started_ms < SCAN_TIMEOUT_MS) return;

  if (n >= 0) _store_scan(n);
  WiFi.scanDelete();
  _scan_running = false;

  if (_pin_check_pending) {
    _pin_check_pending = false;
    if (n >= 0) _maybe_return_to_pin();
  }
  _resume_after_scan();
}

/** No fallback: o fixado reapareceu na varredura com sinal usável? Volta para ele. */
static void _maybe_return_to_pin() {
  if (!_has_pin || !_pin_fallback) return;

  char alvo[WIFI_BSSID_STR_LEN];
  _format_bssid(_pin, alvo);

  int16_t rssi = -127;
  _ensure_scan_mutex();
  xSemaphoreTake(_scan_mutex, portMAX_DELAY);
  for (uint8_t i = 0; i < _scan_n; i++) {
    if (strcmp(_scan[i].bssid, alvo) == 0) rssi = _scan[i].rssi;
  }
  xSemaphoreGive(_scan_mutex);

  if (rssi < PIN_RETURN_MIN_RSSI) return;

  _pin_fallback = false;
  event_log(SEV_INFO, COMP_WIFI, "wifi.ap_pinned",
            "AP fixado %s voltou (RSSI %d dBm); reconectando nele", alvo, (int)rssi);
  _restart_connection("pin-return");
}

static void _apply_pending_pin() {
  if (!_pin_req_pending) return;

  bool tem;
  uint8_t novo[6];
  xSemaphoreTake(_cred_mutex, portMAX_DELAY);
  tem = _pin_req_has;
  memcpy(novo, _pin_req, sizeof(novo));
  _pin_req_pending = false;
  xSemaphoreGive(_cred_mutex);

  _has_pin = tem;
  memcpy(_pin, novo, sizeof(_pin));
  _pin_fallback = false;

  char atual[WIFI_BSSID_STR_LEN];
  bool conectado = wifi_current_bssid(atual);

  if (tem) {
    char alvo[WIFI_BSSID_STR_LEN];
    _format_bssid(_pin, alvo);
    event_log(SEV_INFO, COMP_WIFI, "wifi.ap_pinned", "Ponto de acesso fixado em %s", alvo);
    // Já está nele: nada a fazer. Senão, desassocia e entra no fixado.
    if (!conectado || strcmp(atual, alvo) != 0) _restart_connection("pin");
  } else {
    event_log(SEV_INFO, COMP_WIFI, "wifi.ap_unpinned",
              "Ponto de acesso automatico: o de sinal mais forte");
    // Conectado continua onde está; a escolha pelo sinal vale na próxima vez.
    if (!conectado) _restart_connection("unpin");
  }
}

// ── API ─────────────────────────────────────────────────────────────────────

void wifi_connect() {
  _load_credentials_once();
  _ensure_scan_mutex();
  WiFi.onEvent(_on_wifi_event);
  _start_connect_attempt("startup");
}

static void _log_connected(unsigned long now) {
  char ap[WIFI_BSSID_STR_LEN];
  wifi_current_bssid(ap);
  int rssi = (int)WiFi.RSSI();
  unsigned canal = wifi_current_channel();
  unsigned long fora_s = _offline_since_ms != 0 ? (now - _offline_since_ms) / 1000 : 0;

  if (fora_s >= 5) {
    event_log(SEV_INFO, COMP_WIFI, "wifi.connected",
              "Conectado apos %lus sem Wi-Fi (%u tentativas, ult. motivo %u), RSSI %d dBm, AP %s canal %u",
              fora_s, (unsigned)_attempt_fails, (unsigned)_attempt_reason, rssi, ap, canal);
  } else {
    event_log(SEV_INFO, COMP_WIFI, "wifi.connected",
              "Conectado, IP %s, RSSI %d dBm, AP %s canal %u",
              WiFi.localIP().toString().c_str(), rssi, ap, canal);
  }
  Serial.print("[WiFi] Conectado! IP: ");
  Serial.println(WiFi.localIP());

  _attempt_fails = 0;

  // Fallback, mas o mais forte é o próprio fixado: de volta ao normal.
  if (_has_pin && _pin_fallback) {
    char alvo[WIFI_BSSID_STR_LEN];
    _format_bssid(_pin, alvo);
    if (strcmp(ap, alvo) == 0) _pin_fallback = false;
  }
}

void wifi_check_reconnect() {
  unsigned long now = millis();

  _apply_pending_pin();
  _poll_scan(now);
  // Durante a varredura a estação fica parada de propósito: nada de contar
  // queda nem de tentar conectar por cima dela.
  if (_scan_running) return;

  wl_status_t status = WiFi.status();

  if (status == WL_CONNECTED) {
    if (!_ever_connected) {
      _ever_connected = true;
      _log_connected(now);
    }

    _attempt_count    = 0;
    _offline_since_ms = 0;
    _rssi_before      = (int16_t)WiFi.RSSI();
    _stop_recovery_ap();

    // Usando o mais forte porque o fixado sumiu: de tempos em tempos, procura.
    if (_has_pin && _pin_fallback && now - _pin_check_ms >= WIFI_PIN_RETURN_CHECK_MS) {
      _pin_check_ms = now;
      _pin_check_pending = true;
      _scan_requested = true;
    }
    return;
  }

  if (_ever_connected) {
    // O motivo vem do callback, em outra task, e pode chegar uns
    // milissegundos depois de o status mudar. Espera um pouco por ele.
    bool motivo_chegou = _drops != _drops_logged;
    if (!motivo_chegou && _drop_seen_ms == 0) _drop_seen_ms = now;
    if (motivo_chegou || now - _drop_seen_ms >= DROP_REASON_WAIT_MS) {
      _ever_connected = false;
      _drop_seen_ms   = 0;
      _drops_logged   = _drops;
      uint8_t motivo  = motivo_chegou ? _drop_reason : 0;
      event_log(SEV_WARN, COMP_WIFI, "wifi.disconnected",
                "Conexao perdida: motivo %u (%s), RSSI antes %d dBm",
                (unsigned)motivo,
                motivo ? WiFi.disconnectReasonName((wifi_err_reason_t)motivo) : "?",
                (int)_rssi_before);
      Serial.println("[WiFi] Conexao perdida. Mantendo operacao local.");
    }
  }

  if (_offline_since_ms == 0) {
    _offline_since_ms = now;
  }
  unsigned long fora = now - _offline_since_ms;

  // Fixado, mas ele não aparece: o mais forte, para não ficar sem rede.
  if (_has_pin && !_pin_fallback && fora >= WIFI_PIN_FALLBACK_MS) {
    _pin_fallback = true;
    _pin_check_ms = now;
    char alvo[WIFI_BSSID_STR_LEN];
    _format_bssid(_pin, alvo);
    event_log(SEV_WARN, COMP_WIFI, "wifi.pinned_ap_missing",
              "AP fixado %s sem conectar ha %lu min; usando o mais forte ate ele voltar",
              alvo, fora / 60000UL);
    _restart_connection("pin-fallback");
    return;
  }

  // Preso: desliga e religa o rádio, de novo a cada intervalo igual.
  if (fora >= WIFI_RADIO_RESTART_MS && now - _radio_restart_ms >= WIFI_RADIO_RESTART_MS) {
    _radio_restart_ms = now;
    event_log(SEV_WARN, COMP_WIFI, "wifi.radio_restart",
              "Sem Wi-Fi ha %lu min (%u tentativas, ult. motivo %u); reiniciando o radio",
              fora / 60000UL, (unsigned)_attempt_fails, (unsigned)_attempt_reason);
    _restart_radio();
    return;
  }

  bool timedOut      = fora >= WIFI_RECOVERY_TIMEOUT_MS;
  bool tooManyTries  = _attempt_count >= WIFI_RECOVERY_MAX_ATTEMPTS;

  if (!_recovery_ap_active && (timedOut || tooManyTries)) {
    _start_recovery_ap();
  }

  // O core já religa sozinho a cada falha; isto é só um empurrão periódico
  // para o caso de ele ter desistido (motivos que ele não considera
  // recuperáveis) — chamado enquanto ele tenta, é recusado sem efeito.
  if ((now - _last_attempt_ms) >= WIFI_RECONNECT_INTERVAL_MS) {
    _start_connect_attempt(_recovery_ap_active ? "retry+ap" : "retry");
  }
}

bool wifi_is_connected() {
  return WiFi.status() == WL_CONNECTED;
}

void wifi_reassociate(const char *motivo) {
  event_log(SEV_WARN, COMP_WIFI, "wifi.api_stall_reassoc", "Reassociando o Wi-Fi: %s", motivo);
  _restart_connection("api-stall");
}

bool wifi_recovery_ap_active() {
  return _recovery_ap_active;
}

String wifi_recovery_ap_ssid() {
  _ensure_cred_mutex();
  String out;
  xSemaphoreTake(_cred_mutex, portMAX_DELAY);
  out = _recovery_ap_ssid;
  xSemaphoreGive(_cred_mutex);
  return out;
}

String wifi_configured_ssid() {
  _load_credentials_once();
  String out;
  xSemaphoreTake(_cred_mutex, portMAX_DELAY);
  out = _configured_ssid;
  xSemaphoreGive(_cred_mutex);
  return out;
}

bool wifi_set_credentials(const String &ssid, const String &password) {
  String cleanSsid = ssid;
  cleanSsid.trim();

  if (!_is_valid_ssid(cleanSsid) || !_is_valid_password(password)) {
    Serial.println("[WiFi] Credenciais rejeitadas por validacao");
    return false;
  }

  // Chamado da task do AsyncTCP (o portal de recuperação), não da de rede —
  // é justamente a travessia de núcleo que motivou o mutex.
  _ensure_cred_mutex();
  xSemaphoreTake(_cred_mutex, portMAX_DELAY);
  _configured_ssid     = cleanSsid;
  _configured_password = password;
  xSemaphoreGive(_cred_mutex);
  _prefs_loaded         = true;

  if (_save_credentials(cleanSsid, password)) {
    Serial.printf("[WiFi] Novas credenciais salvas (SSID: %s)\n", cleanSsid.c_str());
  } else {
    Serial.println("[WiFi] Nao foi possivel salvar credenciais na NVS");
  }

  if (_link_up) _voluntary = true;
  WiFi.disconnect();
  _attempt_count    = 0;
  _offline_since_ms = millis();
  _last_attempt_ms  = 0;
  _ever_connected   = false;

  _start_connect_attempt("credentials-update");
  return true;
}

bool wifi_set_pinned_bssid(const String &bssid) {
  String limpo = bssid;
  limpo.trim();
  bool automatico = limpo.length() == 0 || limpo.equalsIgnoreCase("auto");

  uint8_t bytes[6] = {};
  if (!automatico && !_parse_bssid(limpo.c_str(), bytes)) return false;

  char normalizado[WIFI_BSSID_STR_LEN] = "";
  if (!automatico) _format_bssid(bytes, normalizado);

  if (_prefs.begin("wifi_cfg", false)) {
    _prefs.putString("bssid", normalizado);
    _prefs.end();
  } else {
    Serial.println("[WiFi] Nao foi possivel salvar o AP fixado na NVS");
  }

  _ensure_cred_mutex();
  xSemaphoreTake(_cred_mutex, portMAX_DELAY);
  _pin_req_has = !automatico;
  memcpy(_pin_req, bytes, sizeof(_pin_req));
  _pin_req_pending = true;
  xSemaphoreGive(_cred_mutex);
  return true;
}

bool wifi_pinned_bssid(char out[WIFI_BSSID_STR_LEN]) {
  if (!_has_pin) {
    out[0] = '\0';
    return false;
  }
  _format_bssid(_pin, out);
  return true;
}

bool wifi_pin_fallback_active() { return _pin_fallback; }

bool wifi_current_bssid(char out[WIFI_BSSID_STR_LEN]) {
  out[0] = '\0';
  if (WiFi.status() != WL_CONNECTED) return false;
  uint8_t b[6];
  if (WiFi.BSSID(b) == nullptr) return false;
  _format_bssid(b, out);
  return true;
}

uint8_t wifi_current_channel() {
  return WiFi.status() == WL_CONNECTED ? (uint8_t)WiFi.channel() : 0;
}

uint16_t wifi_reconnect_count() { return _drops; }

uint8_t wifi_last_disconnect_reason() { return _drop_reason; }

int16_t wifi_rssi() {
  return WiFi.status() == WL_CONNECTED ? (int16_t)WiFi.RSSI() : -120;
}

bool wifi_rssi_valid() {
  return WiFi.status() == WL_CONNECTED;
}

String wifi_local_ip() {
  return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("");
}

void wifi_request_scan() { _scan_requested = true; }

bool wifi_scan_busy() { return _scan_requested || _scan_running; }

uint8_t wifi_scan_results(WifiApInfo *out, uint8_t max, uint32_t *age_ms) {
  _ensure_scan_mutex();
  xSemaphoreTake(_scan_mutex, portMAX_DELAY);
  uint8_t n = _scan_n < max ? _scan_n : max;
  if (n > 0) memcpy(out, _scan, sizeof(WifiApInfo) * n);
  if (age_ms != nullptr) {
    *age_ms = _scan_done_ms == 0 ? UINT32_MAX : (uint32_t)(millis() - _scan_done_ms);
  }
  xSemaphoreGive(_scan_mutex);
  return n;
}
