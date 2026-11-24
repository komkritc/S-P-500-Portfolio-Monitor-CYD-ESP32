// OTAManager.cpp - firmware update over Wi-Fi (ESP32 port of the SmallTV original).
#include "OTAManager.h"
#include <ArduinoOTA.h>
#include <Update.h>
#include "../storage/StorageManager.h"
#include "../finance/MarketData.h"
#include "WiFiManager.h"
#include "../DisplayUI.h"
#include "../core/PlatformShim.h"

namespace ota {

static const uint32_t MIN_FREE_BLOCK = 6000;  // Updater's buffer + headroom
static const uint32_t STALL_MS = 30000;

static bool s_active = false;
static uint32_t s_lastMs = 0;
static char s_err[48] = "";
static bool s_authOk = false;
static uint32_t s_restartAt = 0;
static bool s_restart = false;
static bool s_otaStarted = false;
static String s_host, s_pass;

// display:: equivalents on the CYD UI (boot banner doubles as the OTA screen)
static void showOta(const char *src)     { char m[48]; snprintf(m, sizeof m, "Updating: %s", src); UI::bootBanner(10, m); }
static void otaProgress(int pct)         { char m[48]; snprintf(m, sizeof m, "Updating %d%%", pct); UI::bootBanner(10 + pct * 80 / 100, m); }
static void otaResult(bool ok, const char *why) {
  if (ok) UI::bootBanner(92, "Update done, rebooting...");
  else { char m[64]; snprintf(m, sizeof m, "Update failed: %s", why); UI::bootBanner(99, m); }
}

const char *lastError() { return s_err; }
bool active() { return s_active; }
bool restartPending() { return s_restart; }

void scheduleRestart(uint32_t delayMs) { s_restart = true; s_restartAt = millis() + delayMs; }

void restartLoop() {
  if (!s_restart || (int32_t)(millis() - s_restartAt) < 0) return;
  if (storage::busy()) return;               // never reset in the middle of a flash write
  market::flushCache();
  Serial.println(F("[sys] restarting"));
  Serial.flush();
  ESP.restart();
}

static void begin_(const char *src) {
  s_active = true;
  s_lastMs = millis();
  market::abortRequests();
  Serial.printf("[ota] start (%s) heap=%u block=%u\n", src, (unsigned)ESP.getFreeHeap(), (unsigned)maxFreeBlock());
  showOta(src);
  otaProgress(0);
}

static void fail(const char *why) {
  Serial.printf("[ota] FAIL %s\n", why);
  strlcpy(s_err, why, sizeof s_err);
  if (s_active) { otaResult(false, why); }
  s_active = false;
  UI::invalidate();
}

void webUpload(WebServer &server, bool authorized) {
  HTTPUpload &up = server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START: {
      s_err[0] = 0;
      s_authOk = authorized;
      if (!s_authOk) { strlcpy(s_err, "unauthorized", sizeof s_err); return; }
      if (storage::busy()) { strlcpy(s_err, "storage busy, try again", sizeof s_err); return; }
      begin_("web upload");
      uint32_t maxSpace = ESP.getFreeSketchSpace();
      if (!Update.begin(maxSpace, U_FLASH)) { String e = Update.errorString(); strlcpy(s_err, e.c_str(), sizeof s_err); }
      break;
    }
    case UPLOAD_FILE_WRITE:
      if (!s_authOk || s_err[0]) return;
      s_lastMs = millis();
      if (Update.write(up.buf, up.currentSize) != up.currentSize) { String e = Update.errorString(); strlcpy(s_err, e.c_str(), sizeof s_err); return; }
      {   // progress: the POST body is slightly bigger than the image (multipart
          // framing), so the bar runs a touch "slow". ESP32 WebServer exposes the
          // request header, not a contentLength() accessor.
        size_t total = (size_t)server.header("Content-Length").toInt();
        if (total) otaProgress((int)((uint64_t)(up.totalSize + up.currentSize) * 100 / total));
      }
      break;
    case UPLOAD_FILE_END:
      if (!s_authOk || s_err[0]) return;
      if (Update.end(true)) otaProgress(100);
      else { String e = Update.errorString(); strlcpy(s_err, e.c_str(), sizeof s_err); }
      break;
    case UPLOAD_FILE_ABORTED:
      if (Update.isRunning()) Update.end();
      if (!s_err[0]) strlcpy(s_err, "upload aborted", sizeof s_err);
      fail(s_err);
      break;
  }
}

void webDone(WebServer &server, bool authorized) {
  if (!authorized) { server.requestAuthentication(BASIC_AUTH, "unauthorized"); return; }
  if (s_err[0]) {
    if (Update.isRunning()) Update.end();
    char msg[80]; snprintf(msg, sizeof msg, "update failed: %s", s_err);
    server.send(500, "text/plain", msg);
    char why[48]; strlcpy(why, s_err, sizeof why);
    fail(why);
    return;
  }
  if (!s_active) { server.send(400, "text/plain", "update failed: no file"); return; }
  server.send(200, "text/plain", "OK, rebooting");     // reply first, restart afterwards
  otaResult(true, "");
  Serial.println(F("[ota] done"));
  s_active = false;
  scheduleRestart(800);
}

void begin(const char *hostname, const char *password) {
  s_host = hostname;
  s_pass = password;
}

static void startArduinoOTA() {
  ArduinoOTA.setHostname(s_host.c_str());              // port stays at the ESP32 default 3232
  if (s_pass.length()) ArduinoOTA.setPassword(s_pass.c_str());
  ArduinoOTA.onStart([]() { if (storage::busy()) return; begin_("ArduinoOTA"); });   // busy: skip the UI, the update still streams
  ArduinoOTA.onProgress([](unsigned int d, unsigned int t) { s_lastMs = millis(); if (t) otaProgress((int)((uint64_t)d * 100 / t)); });
  ArduinoOTA.onEnd([]() { market::flushCache(); otaProgress(100); otaResult(true, ""); });  // core restarts right after onEnd
  ArduinoOTA.onError([](ota_error_t e) {
    fail(e == OTA_AUTH_ERROR ? "auth failed" : e == OTA_BEGIN_ERROR ? "begin failed" : e == OTA_CONNECT_ERROR ? "connect failed" : e == OTA_RECEIVE_ERROR ? "receive failed" : e == OTA_END_ERROR ? "end failed" : "error");
  });
  ArduinoOTA.begin();   // registers its own _arduino mDNS service under WiFiManager's name
}

void loop() {
  if (!s_otaStarted && wifimgr::mdnsUp()) { startArduinoOTA(); s_otaStarted = true; }
  ArduinoOTA.handle();
  if (s_active && (millis() - s_lastMs) > STALL_MS) {   // a web upload whose client vanished must not freeze the device
    if (Update.isRunning()) Update.end();
    fail("upload stalled");
  }
}

}  // namespace ota
