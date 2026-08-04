#include "wifi_manager.h"
#include "config.h"
#include "log_manager.h"
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

static bool _is_valid_ssid(const String &ssid) {
  return ssid.length() > 0 && ssid.length() <= 32;
}

static bool _is_valid_password(const String &password) {
  // Senha WPA2: 8..63; rede aberta: 0
  return password.length() == 0 || (password.length() >= 8 && password.length() <= 63);
}

static void _load_credentials_once() {
  if (_prefs_loaded) return;

  _configured_ssid     = WIFI_SSID;
  _configured_password = WIFI_PASSWORD;

  if (_prefs.begin("wifi_cfg", true)) {
    String savedSsid = _prefs.getString("ssid", "");
    String savedPass = _prefs.getString("pass", "");
    _prefs.end();

    savedSsid.trim();
    if (_is_valid_ssid(savedSsid) && _is_valid_password(savedPass)) {
      _configured_ssid     = savedSsid;
      _configured_password = savedPass;
      Serial.printf("[WiFi] Credenciais carregadas da NVS (SSID: %s)\n", _configured_ssid.c_str());
    } else {
      Serial.println("[WiFi] Sem credencial válida na NVS. Usando config.h");
    }
  } else {
    Serial.println("[WiFi] Falha ao abrir NVS. Usando credenciais de config.h");
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

static void _start_recovery_ap() {
  if (_recovery_ap_active) return;

  _recovery_ap_ssid = _build_recovery_ap_ssid();
  WiFi.mode(WIFI_AP_STA);

  size_t otaPassLen = strlen(OTA_PASSWORD);
  if (otaPassLen < 8 || otaPassLen > 63) {
    Serial.println("[WiFi] OTA_PASSWORD invalido para WPA2 (use 8..63 chars). AP de recuperacao nao iniciado.");
    return;
  }

  bool started = WiFi.softAP(_recovery_ap_ssid.c_str(), OTA_PASSWORD);

  if (!started) {
    Serial.println("[WiFi] Falha ao iniciar AP de recuperacao");
    return;
  }

  _recovery_ap_active = true;
  log_eventf("[WiFi] AP recuperacao ativo: %s", _recovery_ap_ssid.c_str());
  Serial.printf("[WiFi] AP de recuperacao ativo: SSID=%s IP=%s\n",
                _recovery_ap_ssid.c_str(),
                WiFi.softAPIP().toString().c_str());
  Serial.println("[WiFi] Acesse /wifi-setup e autentique com login/senha OTA");
}

static void _stop_recovery_ap() {
  if (!_recovery_ap_active) return;

  WiFi.softAPdisconnect(true);
  // Retorna explicitamente para STA para reduzir superfície de rede e consumo.
  WiFi.mode(WIFI_STA);
  _recovery_ap_active = false;
  _recovery_ap_ssid   = "";
  log_event("[WiFi] AP recuperacao desativado");
  Serial.println("[WiFi] AP de recuperacao desativado");
}

static void _start_connect_attempt(const char* reason) {
  _load_credentials_once();
  _configure_static_ip_once();

  if (!_is_valid_ssid(_configured_ssid)) {
    // Evita loop de tentativas em alta frequência quando SSID está inválido.
    _last_attempt_ms = millis();
    Serial.println("[WiFi] SSID invalido. Aguardando nova configuracao.");
    return;
  }

  WiFi.mode(_recovery_ap_active ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(_configured_ssid.c_str(), _configured_password.c_str());

  _last_attempt_ms = millis();
  if (_offline_since_ms == 0) _offline_since_ms = _last_attempt_ms;
  _attempt_count++;

  Serial.printf("[WiFi] Tentativa #%u (%s) SSID=%s\n",
                (unsigned)_attempt_count,
                reason,
                _configured_ssid.c_str());
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
      log_eventf("[WiFi] Conectado: %s", WiFi.localIP().toString().c_str());
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
    log_event("[WiFi] Conexao perdida");
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
  return _recovery_ap_ssid;
}

String wifi_configured_ssid() {
  _load_credentials_once();
  return _configured_ssid;
}

bool wifi_set_credentials(const String &ssid, const String &password) {
  String cleanSsid = ssid;
  cleanSsid.trim();

  if (!_is_valid_ssid(cleanSsid) || !_is_valid_password(password)) {
    Serial.println("[WiFi] Credenciais rejeitadas por validacao");
    return false;
  }

  _configured_ssid     = cleanSsid;
  _configured_password = password;
  _prefs_loaded        = true;

  if (_save_credentials(_configured_ssid, _configured_password)) {
    Serial.printf("[WiFi] Novas credenciais salvas (SSID: %s)\n", _configured_ssid.c_str());
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
