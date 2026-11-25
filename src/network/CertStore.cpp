// CertStore.cpp - device side of the CA bundle. Pure helpers live in core/PemUtil.h (host-tested).
//
// Install protocol (never breaks the active bundle):
//   pemSane -> write /ca/bundle.new -> fsync-equivalent close -> LIVE test fetch to Yahoo using
//   the NEW roots -> only then rename over /ca/bundle.pem. Any failure leaves the previous
//   bundle (or the built-in roots) active. Power loss mid-install only leaves an orphan .new,
//   which is removed at boot. There is no code path that disables verification.
#include "CertStore.h"
#include "certs_builtin.h"
#include "../core/PemUtil.h"
#include "../finance/YahooFinance.h"
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>

namespace certs {

static const char *CA_DIR  = "/ca";
static const char *CA_PATH = "/ca/bundle.pem";
static const char *CA_NEW  = "/ca/bundle.new";
static const size_t PEM_MAX = 8192;          // uploaded bundle cap
static const uint8_t MAX_ROOTS = 8;
static const uint32_t LIVE_MIN_BLOCK = 16000;   // same floor as the normal Yahoo client

static char *s_cache = nullptr;              // heap copy of the custom bundle (cached; invalidated on install/reset)
static size_t s_cacheLen = 0;

// ------------------------------------------------------------------ pure re-exports
bool pemSane(const char *p, size_t l) { return pem::sane(p, l, PEM_MAX, MAX_ROOTS); }
uint8_t pemCount(const char *p)       { return pem::count(p, p ? strlen(p) : 0); }
uint32_t pemHash(const char *p, size_t l) { return pem::fnv(p, l); }

// ------------------------------------------------------------------ boot hygiene
// Called from storage setup: a .new left behind by a power cut never became active.
bool bootSweep() {
  if (LittleFS.exists(CA_NEW)) { LittleFS.remove(CA_NEW); Serial.println(F("[certs] removed an interrupted bundle upload")); return true; }
  return false;
}

static void dropCache() { free(s_cache); s_cache = nullptr; s_cacheLen = 0; }

// ------------------------------------------------------------------ active bundle
const char *active() {
  if (s_cache) return s_cache;
  if (!LittleFS.exists(CA_PATH)) return CA_BUNDLE;
  File f = LittleFS.open(CA_PATH, "r");
  if (!f) return CA_BUNDLE;
  size_t sz = f.size();
  if (sz < 64 || sz > PEM_MAX) { f.close(); return CA_BUNDLE; }
  char *buf = (char *)malloc(sz + 1);
  if (!buf) { f.close(); return CA_BUNDLE; }
  size_t got = f.readBytes(buf, sz);
  f.close();
  if (got != sz || !pem::sane(buf, sz, PEM_MAX, MAX_ROOTS)) { free(buf); return CA_BUNDLE; }
  buf[sz] = 0;
  s_cache = buf; s_cacheLen = sz;
  return s_cache;
}

const char *sourceName() { return s_cache || LittleFS.exists(CA_PATH) ? "custom" : "builtin"; }

// ------------------------------------------------------------------ live verification
// One synchronous test request (2-3 s, admin-only action, LCD/web keep running afterwards).
// Returns HTTP 200 AND a body that actually contains a chart payload.
static bool liveVerify(const char *pemText, char *err, size_t errN) {
  if (WiFi.status() != WL_CONNECTED) { snprintf(err, errN, "no Wi-Fi for the live test"); return false; }
  if (heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) < LIVE_MIN_BLOCK) {
    snprintf(err, errN, "low memory for the live test"); return false;
  }
  if (ESP.getFreeHeap() < 20000) { snprintf(err, errN, "heap floor"); return false; }
  WiFiClientSecure c;
  c.setCACert(pemText);
  c.setTimeout(10000);
  if (!c.connect("query1.finance.yahoo.com", 443)) {
    char d[40] = ""; c.lastError(d, sizeof d);
    snprintf(err, errN, "TLS refused by the new roots (ssl %d: %s)", c.lastError(d, sizeof d), d);
    c.stop(); return false;
  }
  c.print("GET /v8/finance/chart/%5EGSPC?range=1d&interval=1d HTTP/1.0\r\n"
          "Host: query1.finance.yahoo.com\r\nUser-Agent: Mozilla/5.0\r\n"
          "Accept: application/json\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n");
  bool ok200 = false, hasPayload = false;
  char line[128]; size_t ln = 0; int state = 0;   // 0 headers, 1 body
  uint32_t t0 = millis();
  while (millis() - t0 < 8000 && !(ok200 && hasPayload)) {
    int ch = c.read();
    if (ch < 0) { if (!c.connected()) break; continue; }   // no data yet: wait for the deadline
    if (ch == '\n') {
      line[ln] = 0;
      if (ln && line[ln - 1] == '\r') line[--ln] = 0;
      if (state == 0) {
        if (ln == 0) state = 1;
        else if (ln >= 12 && !strncmp(line, "HTTP/1.0 200", 12)) ok200 = true;
      } else if (ln && (strstr(line, "\"result\"") || strstr(line, "\"timestamp\""))) {
        hasPayload = true;
      }
      ln = 0;
    } else if (ln + 1 < sizeof line) line[ln++] = (char)ch;
    else {                                            // chart JSON is one long line: rolling window
      line[ln] = 0;
      if (strstr(line, "\"result\"") || strstr(line, "\"timestamp\"")) hasPayload = true;
      static const size_t KEEP = 16;
      memmove(line, line + ln - KEEP, KEEP); ln = KEEP;
      line[ln] = 0;
    }
  }
  c.stop();
  if (!ok200) { snprintf(err, errN, "live test: no HTTP 200 from Yahoo"); return false; }
  if (!hasPayload) { snprintf(err, errN, "live test: Yahoo payload missing"); return false; }
  return true;
}

// ------------------------------------------------------------------ install / reset
bool installNew(const char *text, size_t len, char *err, size_t errN) {
  err[0] = 0;
  if (!pem::sane(text, len, PEM_MAX, MAX_ROOTS)) { snprintf(err, errN, "not a usable PEM bundle"); return false; }
  yahoo::abort();                                    // one in-flight TLS connection at a time
  if (!LittleFS.exists(CA_DIR)) LittleFS.mkdir(CA_DIR);
  if (LittleFS.exists(CA_NEW)) LittleFS.remove(CA_NEW);
  File f = LittleFS.open(CA_NEW, "w");
  if (!f) { snprintf(err, errN, "cannot write %s", CA_NEW); return false; }
  size_t put = f.write((const uint8_t *)text, len);
  f.flush(); f.close();
  if (put != len) { LittleFS.remove(CA_NEW); snprintf(err, errN, "short write"); return false; }

  if (!liveVerify(text, err, errN)) {                // PROOF before activation; old bundle stays
    LittleFS.remove(CA_NEW);
    return false;
  }
  if (LittleFS.exists(CA_PATH)) LittleFS.remove(CA_PATH);
  if (!LittleFS.rename(CA_NEW, CA_PATH)) {
    LittleFS.remove(CA_NEW);
    snprintf(err, errN, "rename failed - previous bundle kept");
    dropCache();
    return false;
  }
  dropCache();
  Serial.printf("[certs] custom bundle active: %u roots hash %08X\n",
                (unsigned)pem::count(text, len), (unsigned)pem::fnv(text, len));
  return true;
}

bool resetToBuiltin(char *err, size_t errN) {
  err[0] = 0;
  dropCache();
  if (LittleFS.exists(CA_PATH)) {
    if (!LittleFS.remove(CA_PATH)) { snprintf(err, errN, "cannot remove the custom bundle"); return false; }
  }
  return true;
}

// ------------------------------------------------------------------ status
void statusJson(JsonWriter &w) {
  bool custom = LittleFS.exists(CA_PATH);
  const char *src = active();                        // loads/caches if custom
  w.key("certs").beginObj()
   .kStr("source", custom ? "custom" : "builtin")
   .kInt("roots", pem::count(src, src ? strlen(src) : 0))
   .kInt("hash", src ? pem::fnv(src, strlen(src)) : 0)
   .endObj();
}
}
