#include "ota_manager.h"
#include "app.h"
#include "clock.h"
#include "config.h"
#include "console.h"
#include "debuglog.h"
#include "link.h"
#include "wifi_manager.h"
#include <ESPAsyncWebServer.h>
#define ELEGANTOTA_USE_ASYNC_WEBSERVER 1
#include <ElegantOTA.h>
#include <WiFi.h>

static AsyncWebServer _server(80);
static unsigned long  _ota_progress_ms = 0;
// Escritos na tarefa do servidor, lidos no loop.
static volatile bool     _ota_ativo = false;
static volatile uint32_t _ota_visto_ms = 0;
// Upload que morre no meio (navegador fechado, Wi-Fi caiu) pode não chamar o
// onEnd: sem progresso por este tempo, o bloqueio cai — a agenda não pode
// ficar parada por causa de uma atualização que não vai acontecer.
static const uint32_t OTA_SILENCIO_MS = 60000;

static String _status_text() {
  String out;
  out.reserve(512);
  out += "device_id: " DEVICE_ID "\n";
  out += "fw_version: " FW_VERSION "\n";
  out += "uptime_ms: " + String(millis()) + "\n";
  out += "reset: " + String(log_reset_reason()) + "\n";
  out += "wifi_connected: " + String(wifi_is_connected() ? "true" : "false") + "\n";
  out += "ip: " + (wifi_is_connected() ? WiFi.localIP().toString() : String("--")) + "\n";
  out += "mac: " + WiFi.macAddress() + "\n";
  out += "rssi_dbm: " + String(wifi_is_connected() ? WiFi.RSSI() : 0) + "\n";
  out += "reconnects: " + String(wifi_reconnect_count()) + "\n";

  // Leitura de outra tarefa: um valor pode sair de um instante antes ou
  // depois, nunca algo que mude o módulo.
  char b[96];
  if (clock_valid()) {
    CivilTime t = epoch_to_civil(clock_now());
    snprintf(b, sizeof(b), "clock: %04u-%02u-%02u %02u:%02u:%02u\n", t.year, t.month, t.day, t.hour,
             t.minute, t.second);
  } else {
    snprintf(b, sizeof(b), "clock: --\n");
  }
  out += b;
  snprintf(b, sizeof(b), "rtc: %s  ntp: %s\n", clock_rtc_present() ? "ok" : "ausente",
           clock_ntp_synced() ? "ok" : "--");
  out += b;
  out += "link: " + String(link_connected() ? "conectado" : "fora do fio") + "\n";
  const FeederConfig& c = app_config();
  snprintf(b, sizeof(b), "schedule: %02u h e %02u h, %u graos, auto %s\n", c.hour1, c.hour2, c.grains,
           c.auto_enabled ? "sim" : "nao");
  out += b;
  const Calibration& k = app_calibration();
  snprintf(b, sizeof(b), "calibration: repouso %u, despejo %u%s\n", k.rest_deg, k.dump_deg,
           k.valid ? "" : " (NAO CALIBRADO)");
  out += b;
  snprintf(b, sizeof(b), "meals_24h: %u de %u\n", app_meals_24h(), MEALS_PER_24H);
  out += b;
  return out;
}

void ota_manager_init() {
  _server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain; charset=utf-8", _status_text());
  });

  // Depuração sem o cabo USB, com o mesmo login do OTA: quem pode gravar um
  // firmware pode ler o log e mandar comandos do console.
  _server.on("/log", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate(OTA_USERNAME, OTA_PASSWORD)) return request->requestAuthentication();
    uint32_t desde = 0;
    if (request->hasParam("desde")) desde = strtoul(request->getParam("desde")->value().c_str(), nullptr, 10);
    AsyncWebServerResponse *r = request->beginResponse(200, "text/plain; charset=utf-8", log_recent(desde));
    r->addHeader("X-Log-Proxima", String(log_next_seq()));
    request->send(r);
  });

  _server.on("/console", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->authenticate(OTA_USERNAME, OTA_PASSWORD)) return request->requestAuthentication();
    if (!request->hasParam("cmd", true)) {
      return request->send(400, "text/plain; charset=utf-8", "falta o campo cmd\n");
    }
    if (!console_enqueue(request->getParam("cmd", true)->value().c_str())) {
      return request->send(409, "text/plain; charset=utf-8",
                           "recusado: vazio, longo demais, ou o anterior ainda nao rodou\n");
    }
    request->send(202, "text/plain; charset=utf-8", "na fila; a saida vai para o log (GET /log)\n");
  });

  ElegantOTA.begin(&_server, OTA_USERNAME, OTA_PASSWORD);

  // Só a marcação aqui: quem desliga motor e servo é o loop, que é o dono
  // deles (app_update).
  ElegantOTA.onStart([]() {
    _ota_visto_ms = millis();
    _ota_ativo = true;
    Log.println("[OTA] Atualizacao de firmware iniciada");
  });

  ElegantOTA.onProgress([](size_t current, size_t total) {
    _ota_visto_ms = millis();
    if (millis() - _ota_progress_ms > 1000) {
      _ota_progress_ms = millis();
      Log.printf("[OTA] Progresso: %u / %u bytes (%.0f%%)\n",
                    current, total, (float)current / total * 100.0f);
    }
  });

  ElegantOTA.onEnd([](bool success) {
    if (!success) _ota_ativo = false;   // sem reinício: o módulo segue como estava
    Log.println(success ? "[OTA] Concluido com sucesso! Reiniciando..."
                            : "[OTA] Falha na atualizacao.");
  });

  _server.begin();
  Log.println("[WebServer] Servidor iniciado na porta 80");
}

void ota_manager_loop() {
  ElegantOTA.loop();
}

bool ota_in_progress() {
  if (!_ota_ativo) return false;
  if (millis() - _ota_visto_ms < OTA_SILENCIO_MS) return true;
  _ota_ativo = false;
  Log.println("[OTA] Sem progresso ha 60 s; atualizacao dada por abandonada");
  return false;
}
