#include "store.h"

#include <Arduino.h>
#include <Preferences.h>

#include "debuglog.h"

static Preferences _prefs;
static bool _ok = false;

// A versão do formato vai junto: um bloco gravado por outro firmware, de
// tamanho ou formato diferente, é descartado em vez de lido torto.
static const uint8_t VERSAO = 1;

struct CfgBlob {
  uint8_t      versao;
  FeederConfig cfg;
};
struct CalBlob {
  uint8_t     versao;
  Calibration cal;
};
struct HistBlob {
  uint8_t versao;
  History hist;
};

template <typename T>
static bool _load(const char* chave, T& out) {
  if (!_ok || _prefs.getBytesLength(chave) != sizeof(T)) return false;
  return _prefs.getBytes(chave, &out, sizeof(T)) == sizeof(T) && out.versao == VERSAO;
}

template <typename T>
static void _save(const char* chave, const T& in) {
  if (!_ok) return;
  if (_prefs.putBytes(chave, &in, sizeof(T)) != sizeof(T)) {
    Log.printf("[NVS] Falha ao gravar '%s'\n", chave);
  }
}

void store_init() {
  _ok = _prefs.begin("alimentador", false);
  if (!_ok) Log.println("[NVS] Nao abriu; usando o padrao de fabrica, sem gravar");
}

FeederConfig store_load_config() {
  const FeederConfig fabrica = {DEFAULT_HOUR1, DEFAULT_HOUR2, DEFAULT_GRAINS, DEFAULT_AUTO};
  CfgBlob b;
  if (!_load("agenda", b)) return fabrica;
  const FeederConfig& c = b.cfg;
  if (c.hour1 > 23 || c.hour2 > 23 || c.grains < GRAINS_MIN || c.grains > GRAINS_MAX) return fabrica;
  return c;
}

void store_save_config(const FeederConfig& cfg) {
  CfgBlob b = {VERSAO, cfg};
  _save("agenda", b);
}

Calibration store_load_calibration() {
  const Calibration fabrica = {SERVO_DEFAULT_REST, SERVO_DEFAULT_DUMP, false};
  CalBlob b;
  if (!_load("calibracao", b)) return fabrica;
  const Calibration& c = b.cal;
  if (c.rest_deg > 180 || c.dump_deg > 180 || c.rest_deg == c.dump_deg) return fabrica;
  return c;
}

void store_save_calibration(const Calibration& cal) {
  CalBlob b = {VERSAO, cal};
  _save("calibracao", b);
}

bool store_load_history(History& h) {
  HistBlob b;
  if (!_load("historico", b)) {
    h = {};
    return false;
  }
  h = b.hist;
  if (h.meals_next >= MEALS_HISTORY) h.meals_next = 0;
  return true;
}

void store_save_history(const History& h) {
  HistBlob b = {VERSAO, h};
  _save("historico", b);
}

void store_factory_reset() {
  if (_ok) _prefs.clear();
}
