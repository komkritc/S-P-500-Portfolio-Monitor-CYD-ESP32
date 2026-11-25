# TLS trust roots — how this device verifies Yahoo Finance

Since fw 1.3.0 every HTTPS request to Yahoo Finance is **fully certificate-verified**.
There is no `setInsecure()` anywhere in `src/` and there never will be: if verification
fails, the data simply doesn't load. That is the intended failure mode.

## The chain in production

    ESP32 (BearSSL)  <-- TLS -->  query1/query2.finance.yahoo.com
    leaf: *.www.yahoo.com
      -> DigiCert Global G2 TLS RSA SHA256 2020 CA1 (intermediate, sent by server)
      -> DigiCert Global Root G2                    (root, must be in our bundle)

Both endpoints present the identical chain, so the device only ever needs the **root**
in its bundle. Roots do not rotate with leaf renewals: Root G2 has been stable since
2013. Yahoo re-keys a root only on a ~decadal scale.

## Two-tier trust

| Tier | Where | Size | How it updates |
|------|-------|------|----------------|
| Built-in bundle | `src/network/certs_builtin.h` (PROGMEM, 3 roots: DigiCert Global Root G2, DigiCert TLS RSA4096 Root G5, ISRG Root X1) | 0 RAM bytes | firmware OTA |
| Custom bundle | `/ca/bundle.pem` on LittleFS | only the active copy is heap-cached (~5.5 KB) | web API, in the field, no reflash |

`GET /api/certs` reports which is live: `{"source":"builtin"|"custom","roots":n,"hash":...}`.

## Installing a new bundle from the field (OTA-updatable, no reflash)

1. Produce a PEM file with 1..8 self-signed root certificates (PEM, `-----BEGIN CERTIFICATE-----`).
2. `POST /api/certs/install` with the file as `text/plain` body (authenticated like every mutation).
3. The device **live-tests the new bundle before activating it**: it performs a real TLS fetch
   of Yahoo with the candidate roots. Only if that succeeds is `/ca/bundle.pem` swapped in
   (written to `/ca/bundle.new`, then atomic rename). A bundle that breaks connectivity can
   never take effect — the API answers `TLS refused by the new roots (ssl -9984: ...)`.
4. `POST /api/certs/reset` removes the custom bundle and falls back to the built-in one.

An interrupted upload can never activate a half-written bundle (`.new` is never renamed;
`certs::bootSweep()` deletes orphans at boot).

## Refreshing the built-in bundle (maintainer, when a root really does change)

    python3 tools/fetch_roots.py          # downloads curl.se CA store, extracts the roots,
                                          # verifies each is self-signed AND that the live
                                          # query1/query2 chains anchor to them,
                                          # then regenerates src/network/certs_builtin.h
    pio run -e cyd_ota -t upload --upload-port sp500-32.local

If Yahoo ever moves to a root not in the bundle, the **field install path above recovers it
without any reflash**: fetch the new root from the CA vendor, POST it, done.

## Clock gate

A certificate is only meaningful if the clock is right, so the market scheduler refuses to
start any HTTPS request until NTP has synced (`/api/status` → `data.blocked:
"clock not synced"`, `tls.clock`). The NTP client uses a plain time protocol — it consumes
no TLS trust — and sync happens a few seconds after Wi-Fi connects.

## Threat model, honestly

- What this defeats: any on-path attacker on the Wi-Fi/LAN impersonating Yahoo (spoofed DNS,
  ARP spoofing, evil AP). They cannot inject prices without a CA-signed *.yahoo.com cert.
- What it does not claim: the ESP32 cannot check OCSP/CRL revocation with BearSSL.
  Pinned *roots* + full chain verification is the practical embedded ceiling; it is the
  same trust level as a browser that does not enforce OCSP stapling.
- TLS handshake cost, measured live: ~1.5 s, ~45 KB heap peak, min-block 110580 → 94196.
