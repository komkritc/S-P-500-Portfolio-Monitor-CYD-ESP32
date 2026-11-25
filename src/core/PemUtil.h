// PemUtil.h - pure (host-testable) helpers for PEM certificate bundles. No FS/network here;
// the device side lives in network/CertStore.cpp, the host tests exercise these directly.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace pem {

// Number of "-----BEGIN CERTIFICATE-----" markers in the text.
inline uint8_t count(const char *s, size_t len) {
  static const char BEG[] = "-----BEGIN CERTIFICATE-----";
  const size_t B = sizeof BEG - 1;
  uint8_t n = 0;
  if (!s) return 0;
  for (size_t i = 0; i + B <= len; i++)
    if (s[i] == '-' && !memcmp(s + i, BEG, B)) { n++; i += B - 1; }
  return n;
}

// FNV-1a over the exact bytes: a stable "which bundle is this" id for diagnostics.
inline uint32_t fnv(const char *s, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
  return h;
}

// Structural sanity for an uploaded bundle (NOT cryptographic validity - the live TLS test
// after install is what proves trust). Rules: 1..MAX_ROOTS certs, every BEGIN matched by an
// END in order, body lines are base64 charset only (blank lines tolerated), no NULs, size cap.
inline bool sane(const char *s, size_t len, size_t maxLen, uint8_t maxRoots) {
  if (!s || !len || len > maxLen) return false;
  static const char BEG[] = "-----BEGIN CERTIFICATE-----";
  static const char END[] = "-----END CERTIFICATE-----";
  const size_t B = sizeof BEG - 1, E = sizeof END - 1;
  size_t i = 0; uint8_t n = 0;
  while (i < len) {
    if (s[i] == 0) return false;
    if (i + B <= len && !memcmp(s + i, BEG, B)) {
      if (++n > maxRoots) return false;
      i += B;
      size_t j = i;
      bool inBody = true;
      while (j < len) {
        if (j + E <= len && !memcmp(s + j, END, E)) break;
        char c = s[j];
        if (inBody) {
          bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '+' || c == '/' || c == '=' || c == '\n' || c == '\r';
          if (!ok) return false;
        }
        j++;
      }
      if (j >= len) return false;                       // BEGIN without END
      i = j + E;
      continue;
    }
    i++;
  }
  return n >= 1;
}

}  // namespace pem
