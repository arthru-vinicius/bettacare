#include "device_config.h"

#include <Preferences.h>
#include <math.h>

#include "config.h"
#include "event_log.h"

#define CFG_NS          "bc_cfg"
#define CFG_KEY_BLOB    "cfg"
#define CFG_KEY_VERSION "ver"

/** Falhas de NVS desde o boot — ver `device_config.h`, seção "Saúde da NVS". */
static uint16_t          _nvs_failures = 0;

void nvs_report_failure() {
  if (_nvs_failures < 65535) _nvs_failures++;
}

uint16_t nvs_failure_count() { return _nvs_failures; }

static DeviceConfig      _cfg;
static uint32_t          _version = 0;
static SemaphoreHandle_t _mutex = nullptr;

static void _lock()   { if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY); }
static void _unlock() { if (_mutex) xSemaphoreGive(_mutex); }

static void _load_defaults() {
  _cfg.light_on_hour  = DEFAULT_LIGHT_ON_HOUR;
  _cfg.light_on_min   = DEFAULT_LIGHT_ON_MIN;
  _cfg.light_off_hour = DEFAULT_LIGHT_OFF_HOUR;
  _cfg.light_off_min  = DEFAULT_LIGHT_OFF_MIN;
  _cfg.fan_trigger_c  = DEFAULT_FAN_TRIGGER_C;
  _cfg.fan_off_c      = DEFAULT_FAN_OFF_C;
  _cfg.telemetry_interval_ms = DEFAULT_TELEMETRY_INTERVAL_MS;
  _cfg.heartbeat_interval_ms = DEFAULT_HEARTBEAT_INTERVAL_MS;
}

static bool _valid(const DeviceConfig& c) {
  if (c.light_on_hour > 23 || c.light_off_hour > 23) return false;
  if (c.light_on_min  > 59 || c.light_off_min  > 59) return false;
  if (isnan(c.fan_trigger_c) || isnan(c.fan_off_c)) return false;
  if (c.fan_trigger_c < 15.0f || c.fan_trigger_c > 45.0f) return false;
  if (c.fan_off_c     < 10.0f || c.fan_off_c     > 44.5f) return false;
  // Sem esta folga a ventoinha ligaria e desligaria sem parar.
  if (c.fan_off_c >= c.fan_trigger_c) return false;
  if (c.telemetry_interval_ms < 1000 || c.telemetry_interval_ms > 60000) return false;
  if (c.heartbeat_interval_ms < 10000 || c.heartbeat_interval_ms > 600000) return false;
  return true;
}

void device_config_init() {
  _mutex = xSemaphoreCreateMutex();
  _load_defaults();

  Preferences prefs;
  if (!prefs.begin(CFG_NS, true)) {
    nvs_report_failure();
    event_log(SEV_WARN, COMP_NVS, "nvs.open_failed",
              "Nao foi possivel ler a configuracao; usando padroes de fabrica");
    return;
  }

  DeviceConfig saved;
  size_t lidos = prefs.getBytes(CFG_KEY_BLOB, &saved, sizeof(saved));
  uint32_t ver = prefs.getUInt(CFG_KEY_VERSION, 0);
  prefs.end();

  if (lidos != sizeof(saved) || !_valid(saved)) {
    event_log(SEV_INFO, COMP_NVS, "nvs.no_config",
              "Sem configuracao valida em NVS; usando padroes ate falar com o servidor");
    return;
  }

  _cfg = saved;
  _version = ver;
  event_log(SEV_INFO, COMP_SYSTEM, "system.config_loaded",
            "Config v%lu do cache: luz %s-%s, ventoinha %.1f/%.1fC",
            (unsigned long)ver, device_config_on_time().c_str(),
            device_config_off_time().c_str(), _cfg.fan_trigger_c, _cfg.fan_off_c);
}

DeviceConfig device_config_snapshot() {
  _lock();
  DeviceConfig copia = _cfg;
  _unlock();
  return copia;
}

uint32_t device_config_version() { return _version; }

bool device_config_apply(const DeviceConfig& cfg, uint32_t version) {
  if (!_valid(cfg)) {
    event_log(SEV_ERROR, COMP_API, "api.bad_config",
              "Servidor mandou config invalida (v%lu); mantendo a anterior",
              (unsigned long)version);
    return false;
  }

  // Só a troca da struct precisa ser exclusiva. A gravação em NVS pode levar
  // dezenas de milissegundos e não pode segurar o loop de controle; o log
  // também fica fora, para não inverter a ordem dos mutex.
  _lock();
  _cfg = cfg;
  _version = version;
  DeviceConfig aplicada = _cfg;
  _unlock();

  Preferences prefs;
  if (prefs.begin(CFG_NS, false)) {
    prefs.putBytes(CFG_KEY_BLOB, &aplicada, sizeof(aplicada));
    prefs.putUInt(CFG_KEY_VERSION, _version);
    prefs.end();
  } else {
    // A configuração vale para esta sessão mesmo sem conseguir persistir — só
    // não sobrevive a um reboot sem servidor.
    nvs_report_failure();
    event_log(SEV_WARN, COMP_NVS, "nvs.write_failed",
              "Config v%lu aplicada mas nao persistida", (unsigned long)version);
  }

  event_log(SEV_INFO, COMP_SYSTEM, "system.config_applied",
            "Config v%lu aplicada: luz %s-%s, ventoinha %.1f/%.1fC",
            (unsigned long)version, device_config_on_time().c_str(),
            device_config_off_time().c_str(), _cfg.fan_trigger_c, _cfg.fan_off_c);
  return true;
}

static String _fmt(uint8_t h, uint8_t m) {
  char buf[8];   // o compilador não sabe que h < 24 e m < 60
  snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)h, (unsigned)m);
  return String(buf);
}

String device_config_on_time() {
  DeviceConfig c = device_config_snapshot();
  return _fmt(c.light_on_hour, c.light_on_min);
}

String device_config_off_time() {
  DeviceConfig c = device_config_snapshot();
  return _fmt(c.light_off_hour, c.light_off_min);
}
