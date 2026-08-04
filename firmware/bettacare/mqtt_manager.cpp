#include "mqtt_manager.h"
#include "config.h"
#include "light.h"
#include "fan.h"
#include "temperature.h"
#include "rtc_manager.h"
#include "wifi_manager.h"
#include "log_manager.h"
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <ctype.h>

static WiFiClientSecure _tls;
static PubSubClient     _mqtt(_tls);

// ---------------------------------------------------------------------------
// Estado anterior — publish-on-change
// ---------------------------------------------------------------------------
// _prev_fan_pct == -1 indica "nunca publicado", forçando a publicação inicial.
static bool  _prev_light      = false;
static int   _prev_fan_pct    = -1;
static float _prev_temp       = -999.0f;
static float _prev_fan_trig   = -999.0f;
static float _prev_fan_off    = -999.0f;
static String _prev_rtc_on    = "";
static String _prev_rtc_off   = "";

// ---------------------------------------------------------------------------
// Controle de reconexão não-bloqueante
// ---------------------------------------------------------------------------
static unsigned long _last_reconnect_ms = 0;
static const unsigned long RECONNECT_MS = 10000UL;  // 10 s entre tentativas
static bool _last_connection_ok = false;
static int  _last_fail_rc       = 999;

// ---------------------------------------------------------------------------
// Controle de publicação de logs (debounce 30 s)
// ---------------------------------------------------------------------------
static unsigned long _last_log_publish_ms = 0;
static const unsigned long LOG_DEBOUNCE_MS = 30000UL;

// ---------------------------------------------------------------------------
// Protótipos privados
// ---------------------------------------------------------------------------
static void _reconnect();
static void _on_message(char* topic, byte* payload, unsigned int len);

static const char* _mqtt_state_text(int rc) {
  switch (rc) {
    case -4: return "MQTT_CONNECTION_TIMEOUT";
    case -3: return "MQTT_CONNECTION_LOST";
    case -2: return "MQTT_CONNECT_FAILED (TCP/TLS)";
    case -1: return "MQTT_DISCONNECTED";
    case 0:  return "MQTT_CONNECTED";
    case 1:  return "MQTT_CONNECT_BAD_PROTOCOL";
    case 2:  return "MQTT_CONNECT_BAD_CLIENT_ID";
    case 3:  return "MQTT_CONNECT_UNAVAILABLE";
    case 4:  return "MQTT_CONNECT_BAD_CREDENTIALS";
    case 5:  return "MQTT_CONNECT_UNAUTHORIZED";
    default: return "MQTT_UNKNOWN_STATE";
  }
}

static bool _parse_hhmm(const String &value, uint8_t &hour, uint8_t &minute) {
  if (value.length() != 5) return false;
  if (value[2] != ':') return false;
  if (!isdigit((unsigned char)value[0]) || !isdigit((unsigned char)value[1])) return false;
  if (!isdigit((unsigned char)value[3]) || !isdigit((unsigned char)value[4])) return false;

  int h = (value[0] - '0') * 10 + (value[1] - '0');
  int m = (value[3] - '0') * 10 + (value[4] - '0');
  if (h < 0 || h > 23 || m < 0 || m > 59) return false;

  hour = (uint8_t)h;
  minute = (uint8_t)m;
  return true;
}

// ---------------------------------------------------------------------------
// Callback de mensagens recebidas pelo broker
// ---------------------------------------------------------------------------

/**
 * @brief Roteia mensagens de aquarium/cmd/* para as funções de controle
 *        correspondentes e aciona mqtt_publish_state(true) para publicar o novo
 *        estado imediatamente, sem aguardar o próximo ciclo de mqtt_loop().
 */
static void _on_message(char* topic, byte* payload, unsigned int len) {
  String t = String(topic);

  // Extrai payload como String
  String msg;
  msg.reserve(len);
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];

  if (t == "aquarium/cmd/light" && msg == "toggle") {
    light_toggle();
    Serial.println("[MQTT] Comando recebido: light toggle");

  } else if (t == "aquarium/cmd/fan" && msg == "toggle") {
    fan_toggle_web();
    Serial.println("[MQTT] Comando recebido: fan toggle");

  } else if (t == "aquarium/cmd/fan/speed") {
    int pct = constrain(msg.toInt(), 0, 100);
    fan_set_speed_web(pct);
    Serial.printf("[MQTT] Comando recebido: fan speed %d%%\n", pct);

  } else if (t == "aquarium/cmd/rtc/sync") {
    bool ok = rtc_sync_ntp();
    Serial.printf("[MQTT] Comando recebido: rtc sync (%s)\n", ok ? "OK" : "FALHA");

  } else if (t == "aquarium/cmd/fan/config") {
    DynamicJsonDocument doc(160);
    DeserializationError err = deserializeJson(doc, msg);
    if (err) {
      log_eventf("[MQTT] fan/config JSON invalido: %s", err.c_str());
      Serial.printf("[MQTT] fan/config JSON invalido: %s\n", err.c_str());
      return;
    }

    if (doc["trigger"].isNull() || doc["off"].isNull()) {
      log_event("[MQTT] fan/config sem trigger/off validos");
      Serial.println("[MQTT] fan/config sem trigger/off validos");
      return;
    }

    float trigger = doc["trigger"].as<float>();
    float off     = doc["off"].as<float>();
    if (!fan_set_auto_thresholds(trigger, off)) {
      log_event("[MQTT] fan/config rejeitado por validacao");
      Serial.println("[MQTT] fan/config rejeitado por validacao");
      return;
    }
    Serial.printf("[MQTT] Comando recebido: fan config trigger=%.1f off=%.1f\n", trigger, off);

  } else if (t == "aquarium/cmd/light/schedule") {
    DynamicJsonDocument doc(192);
    DeserializationError err = deserializeJson(doc, msg);
    if (err) {
      log_eventf("[MQTT] light/schedule JSON invalido: %s", err.c_str());
      Serial.printf("[MQTT] light/schedule JSON invalido: %s\n", err.c_str());
      return;
    }

    String on  = doc["on"]  | "";
    String off = doc["off"] | "";
    uint8_t on_h = 0, on_m = 0, off_h = 0, off_m = 0;
    if (!_parse_hhmm(on, on_h, on_m) || !_parse_hhmm(off, off_h, off_m)) {
      log_event("[MQTT] light/schedule rejeitado: horario invalido");
      Serial.println("[MQTT] light/schedule rejeitado: horario invalido");
      return;
    }

    if (!rtc_set_schedule(on_h, on_m, off_h, off_m)) {
      log_event("[MQTT] light/schedule rejeitado por validacao");
      Serial.println("[MQTT] light/schedule rejeitado por validacao");
      return;
    }
    Serial.printf("[MQTT] Comando recebido: light schedule on=%s off=%s\n",
                  on.c_str(), off.c_str());

  } else if (t == "aquarium/cmd/logs/clear") {
    log_clear();
    Serial.println("[MQTT] Buffer de logs limpo via comando");
    return;  // sem publish de estado — apenas confirma

  } else {
    Serial.printf("[MQTT] Topico desconhecido ignorado: %s\n", topic);
    return;
  }

  // Publica o estado atualizado sem esperar o próximo mqtt_loop()
  mqtt_publish_state(true);
}

// ---------------------------------------------------------------------------
// Reconexão não-bloqueante
// ---------------------------------------------------------------------------

/**
 * @brief Tenta (re)conectar ao broker HiveMQ caso necessário.
 *        Respeita um intervalo mínimo de RECONNECT_MS entre tentativas para
 *        não saturar o broker em caso de falha persistente.
 */
static void _reconnect() {
  if (millis() - _last_reconnect_ms < RECONNECT_MS) return;
  _last_reconnect_ms = millis();

  if (!wifi_is_connected()) return;

  // LWT: broker publica "offline" em aquarium/lwt se a conexão cair
  bool ok = _mqtt.connect(
    MQTT_CLIENT_ID,
    MQTT_USER,
    MQTT_PASSWORD,
    "aquarium/lwt",   // LWT topic
    1,                // LWT QoS 1
    true,             // LWT retain
    "offline"         // LWT payload
  );

  if (ok) {
    _mqtt.subscribe("aquarium/cmd/#", 1);  // QoS 1 para garantir entrega
    mqtt_publish_state(true);              // anuncia estado atual assim que (re)conecta

    if (!_last_connection_ok) {
      log_event("[MQTT] Conectado ao HiveMQ");
      Serial.println("[MQTT] Conectado ao HiveMQ Cloud");
    }
    _last_connection_ok = true;
    _last_fail_rc = 0;
  } else {
    int rc = _mqtt.state();
    if (_last_connection_ok || rc != _last_fail_rc) {
      log_eventf("[MQTT] Falha na conexao rc=%d (%s)", rc, _mqtt_state_text(rc));
      Serial.printf("[MQTT] Falha na conexao rc=%d (%s) — tentando em 10s\n",
                    rc, _mqtt_state_text(rc));
    }
    _last_connection_ok = false;
    _last_fail_rc = rc;
  }
}

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------

void mqtt_init() {
  // setInsecure() pula a validação do certificado do servidor.
  // Para um projeto pessoal em rede doméstica é aceitável.
  // Para produção, substituir por: _tls.setCACert(ISRG_ROOT_X1_PEM);
  _tls.setInsecure();
  // Limite de 6 s para o handshake TLS — evita bloquear o loop por longo tempo
  // caso o HiveMQ demore a responder (rede lenta, instabilidade transitória, etc.)
  _tls.setTimeout(6);

  _mqtt.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
  _mqtt.setCallback(_on_message);
  // Buffer: 4096 bytes acomoda o JSON de status (~250 B) e de logs (~3 KB max)
  _mqtt.setBufferSize(4096);

  // Força primeira tentativa imediatamente (sem esperar 10 s)
  _last_reconnect_ms = millis() - RECONNECT_MS;
  _reconnect();

  Serial.println("[MQTT] Modulo inicializado (broker: " MQTT_BROKER_HOST ")");
}

void mqtt_loop() {
  if (!_mqtt.connected()) {
    _reconnect();
  } else {
    _mqtt.loop();
    // Verifica mudanças de estado em todo ciclo — publish_state retorna imediatamente
    // se nada mudou (comparação barata; sem serialização JSON desnecessária)
    mqtt_publish_state(false);
    mqtt_publish_logs();
  }
}

void mqtt_publish_logs() {
  if (!_mqtt.connected())   return;
  if (!log_has_pending())   return;
  // Debounce: aguarda 30 s após o primeiro evento para agrupar a rajada do boot
  if (_last_log_publish_ms != 0 &&
      millis() - _last_log_publish_ms < LOG_DEBOUNCE_MS) return;

  String payload = log_get_json();
  bool ok = _mqtt.publish("aquarium/logs", payload.c_str(), /*retain=*/true);
  if (ok) {
    log_mark_published();
    _last_log_publish_ms = millis();
    Serial.printf("[MQTT] Logs publicados (%u bytes)\n", (unsigned)payload.length());
  }
}

void mqtt_publish_state(bool force) {
  if (!_mqtt.connected()) return;

  bool  light       = light_get_state();
  int   fan         = fan_get_speed_percent();
  float fanTrigger  = fan_get_trigger_c();
  float fanOff      = fan_get_off_c();
  float temp        = temperature_is_fresh() ? temperature_read() : -999.0f;
  String rtcOn      = rtc_get_on_time();
  String rtcOff     = rtc_get_off_time();

  // Verificação barata: retorna antes de serializar se nada mudou
  bool changed = force
              || (_prev_fan_pct < 0)
              || (light != _prev_light)
              || (fan   != _prev_fan_pct)
              || (temp  != -999.0f && fabsf(temp - _prev_temp) >= MQTT_TEMP_THRESHOLD)
              || (fabsf(fanTrigger - _prev_fan_trig) >= 0.01f)
              || (fabsf(fanOff - _prev_fan_off) >= 0.01f)
              || (rtcOn != _prev_rtc_on)
              || (rtcOff != _prev_rtc_off);

  if (!changed) return;

  // Atualiza snapshot anterior
  _prev_light      = light;
  _prev_fan_pct    = fan;
  _prev_fan_trig   = fanTrigger;
  _prev_fan_off    = fanOff;
  _prev_rtc_on     = rtcOn;
  _prev_rtc_off    = rtcOff;
  if (temp != -999.0f) _prev_temp = temp;

  // Serializa — mesmo formato do JSON retornado pelo web_server
  DynamicJsonDocument doc(640);

  JsonObject light_obj    = doc.createNestedObject("light");
  light_obj["on"]         = light;

  JsonObject temp_obj     = doc.createNestedObject("temperature");
  temp_obj["available"]   = temperature_available();
  temp_obj["valid"]       = temperature_is_fresh();
  if (temp != -999.0f) {
    temp_obj["celsius"]   = serialized(String(temp, 1));
  } else {
    temp_obj["celsius"]   = (char*)nullptr;
  }

  JsonObject fan_obj       = doc.createNestedObject("fan");
  fan_obj["on"]            = fan_is_on();
  fan_obj["speed_percent"] = fan;
  fan_obj["trigger_c"]     = serialized(String(fanTrigger, 1));
  fan_obj["off_c"]         = serialized(String(fanOff, 1));

  JsonObject rtc_obj   = doc.createNestedObject("rtc");
  rtc_obj["time"]      = rtc_get_time_str();
  rtc_obj["available"] = rtc_available();
  rtc_obj["on_time"]   = rtcOn;
  rtc_obj["off_time"]  = rtcOff;

  String payload;
  serializeJson(doc, payload);

  bool ok = _mqtt.publish("aquarium/status", payload.c_str(), /*retain=*/true);
  Serial.printf("[MQTT] Estado publicado%s (%u bytes)\n",
                ok ? "" : " [FALHA]", (unsigned)payload.length());
}
