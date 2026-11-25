// YahooFinance.h - ONE in-flight HTTPS request to query1.finance.yahoo.com, driven from loop().
//
//  * HTTP/1.0 request: Yahoo then answers without chunked encoding, so the body can be parsed as it arrives.
//  * Only the TLS handshake blocks (~1-3 s on ESP8266). Header/body reading is sliced: each poll() reads
//    at most a few hundred bytes, so the web server and LCD keep running during the transfer.
//  * The parser is fed straight from the socket: no body buffer, peak heap = BearSSL (~16 KB contiguous).
//  * Certificate verification is ON: roots come from CertStore (built-in PROGMEM bundle or a
//    field-updatable custom bundle). There is NO insecure path. Requests are additionally
//    gated on an NTP-synced clock (cert time windows are checked against the RTC).
//  * Handshake cost is measured per connection (time + heap before/after) and exposed via
//    stats() for /api/status - no assumptions about the TLS footprint.
#pragma once
#include <Arduino.h>
#include "YahooParser.h"

namespace yahoo {

enum Phase : uint8_t { IDLE = 0, RUNNING, DONE, FAILED };

struct Result {
  Phase  phase;
  int    http;                 // HTTP status, 0 if none
  char   error[40];            // short text when FAILED
  bool   rateLimited;          // HTTP 429 / 403
  bool   notFound;             // Yahoo "Not Found" (unknown symbol)
};

// Measured TLS footprint of the last completed/failed start().
struct TlsStats {
  uint32_t hsMs;             // handshake duration
  uint32_t heapBefore;       // free heap just before connect()
  uint32_t heapAfter;        // free heap just after connect() (or at failure)
  uint32_t blockBefore;      // largest free block before connect()
  uint32_t blockAfter;       // largest free block after connect()
  uint16_t certsHash;        // low bits of the active bundle FNV (which roots were used)
  bool     verified;         // always true: verification cannot be disabled
  bool     clockSynced;      // clock state at connect time
};
const TlsStats &stats();

// path: e.g. "/v8/finance/chart/%5EGSPC?range=1d&interval=5m". parser must already be begin()'d and
// stay alive until the request ends. Returns false (and sets error) if it could not start
// (heap too low, Wi-Fi down, request already running).
bool start(const char *path, YahooParser &parser);
void poll();                                  // advance the request; call every loop()
bool busy();
const Result &result();
void abort();                                 // drop the connection (e.g. before OTA / restart)
// Builds "/v8/finance/chart/<enc symbol>?range=..&interval=.." into out.
bool buildRangePath(char *out, size_t n, const char *symbol, const char *range, const char *interval);
bool buildPeriodPath(char *out, size_t n, const char *symbol, uint32_t period1, uint32_t period2);
}
