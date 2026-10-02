#include "wifi_manager.h"
#include "config.h"
#include "event_log.h"
#include <Preferences.h>
#include <WiFi.h>

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

static Preferences _prefs;

/**
 * Protege `_configured_ssid`, `_configured_password` e `_recovery_ap_ssid`
 * (UPGRADE/03, F6). São `String`: atribuir a uma delas libera o buffer antigo
 * e aloca outro. Três contextos tocam este estado — a task de rede (núcleo
 * 0), o `setup()` no boot, e a task do AsyncTCP quando alguém salva
 * credenciais pelo portal de recuperação — e sem proteção, um deles podia
 * seguir um ponteiro que o outro acabou de liberar.
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
static uint16_t      _reconnects         = 0;

static String _configured_ssid;
static String _configured_password;
static String _recovery_ap_ssid;

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

  if (_prefs.begin("wifi_cfg", true)) {
    String savedSsid = _prefs.getString("ssid", "");
    String savedPass = _prefs.getString("pass", "");
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

static void _start_recovery_ap() {
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
  event_log(SEV_WARN, COMP_WIFI, "wifi.ap_mode",
            "AP de recuperacao ativo: %s", ssid.c_str());
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
  WiFi.begin(ssid.c_str(), password.c_str());

  _last_attempt_ms = millis();
  if (_offline_since_ms == 0) _offline_since_ms = _last_attempt_ms;
  _attempt_count++;

  Serial.printf("[WiFi] Tentativa #%u (%s) SSID=%s\n",
                (unsigned)_attempt_count,
                reason,
                ssid.c_str());
}

void wifi_connect() {
  _load_credentials_once();
  _start_connect_attempt("startup");
}

void wifi_check_reconnect() {
  wl_status_t status = WiFi.status();
  unsigned long now = millis();

  if (status == WL_CONNECTED) {
    if (!_ever_connected) {
      _ever_connected = true;
      event_log(SEV_INFO, COMP_WIFI, "wifi.connected",
                "Conectado, IP %s, RSSI %d dBm",
                WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
      Serial.print("[WiFi] Conectado! IP: ");
      Serial.println(WiFi.localIP());
    }

    _attempt_count    = 0;
    _offline_since_ms = 0;
    _stop_recovery_ap();
    return;
  }

  if (_ever_connected) {
    _ever_connected = false;
    _reconnects++;
    event_log(SEV_WARN, COMP_WIFI, "wifi.disconnected", "Conexao perdida");
    Serial.println("[WiFi] Conexao perdida. Mantendo operacao local.");
  }

  if (_offline_since_ms == 0) {
    _offline_since_ms = now;
  }

  bool timedOut      = (now - _offline_since_ms) >= WIFI_RECOVERY_TIMEOUT_MS;
  bool tooManyTries  = _attempt_count >= WIFI_RECOVERY_MAX_ATTEMPTS;

  if (!_recovery_ap_active && (timedOut || tooManyTries)) {
    _start_recovery_ap();
  }

  if ((now - _last_attempt_ms) >= WIFI_RECONNECT_INTERVAL_MS) {
    _start_connect_attempt(_recovery_ap_active ? "retry+ap" : "retry");
  }
}

bool wifi_is_connected() {
  return WiFi.status() == WL_CONNECTED;
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

  WiFi.disconnect();
  _attempt_count    = 0;
  _offline_since_ms = millis();
  _last_attempt_ms  = 0;
  _ever_connected   = false;

  _start_connect_attempt("credentials-update");
  return true;
}

uint16_t wifi_reconnect_count() { return _reconnects; }

int16_t wifi_rssi() {
  return WiFi.status() == WL_CONNECTED ? (int16_t)WiFi.RSSI() : -120;
}

bool wifi_rssi_valid() {
  return WiFi.status() == WL_CONNECTED;
}

String wifi_local_ip() {
  return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("");
}
