# QA report — fw 1.1.9 (no code changed, report only)

> **Status update (as of fw 1.3.0):** A (open auth/OTA) — still open, needs a password on the device.
> B (TLS `setInsecure`) — **fixed in 1.3.0**: full chain verification + field-updatable roots, see
> `docs/CERTS.md` and `docs/TLS-1.3.0.md`. C (FIFO array order) — **fixed in 1.2.0**.
> D (CSV truncation, bucket eviction, seq:0 semantics) — **fixed in 1.2.1**. E/F/G otherwise unchanged.

Scope: full source read of storage/SaleStore + SaleEngine, WebServerManager,
OTAManager, MarketData/YahooFinance/YahooParser, Portfolio/MarketClock/
MarketData, DisplayUI/TouchUI, config + build/test state, and a live probe of
sp500-32.local.

Health checks at report time: unit tests 128/128 pass; preview harness 76
frames / 0 problems; `pio run -e cyd` and OTA build SUCCESS; device reports
fw 1.1.9, heap ~150–206 KB free, clock synced, FS ok.

---

## A. HIGH — the device is currently wide open on the LAN

Live probe: `GET /api/settings` → `"adminPassSet":false`.

Because `authorized()` returns true whenever `adminPass[0]==0`, right now
unauthenticated LAN clients can:

- `POST /api/positions`, `/api/sales/*` (add sells, delete, factory reset!)
- `POST /api/settings` (set a password and lock the real owner out)
- `POST /update` (replace firmware over the browser form)
- **ArduinoOTA on TCP 3232 with NO password** — `ota::begin(hostname,
  g_app.settings.adminPass)` passes adminPass as the OTA password, and
  `startArduinoOTA()` only calls `setPassword()` when it is non-empty.
  Anyone on the LAN can `pio run -t upload` arbitrary firmware.

Fix options (not applied): set an admin password from web Settings; or make
empty-adminPass refuse `POST /update` and require `ArduinoOTA.setPassword`
unconditionally.

Related: `OTA_PASS` in `include/secrets.h` / `secrets.example.h` is **never
referenced by any code** — dead config, and the README tells users to fill it
in. Confusing: real OTA credential == admin password.

## B. MEDIUM — Yahoo TLS is not verified

`YahooFinance::start()` uses `s_cl->setInsecure()` (documented nowhere in the
code). A LAN/MITM attacker can answer the chart/quote requests with forged
prices, which flow straight into the NAV/P&L on LCD and web. Acceptable
trade-off for a home device but should be a comment/README statement, or
better: fingerprint the chain / use `setCACert` bundle.

## C. MEDIUM — "FIFO" sells by array order, not by purchase date

`sale::preview()` builds `lots[]` in `g_app.positions[]` order and consumes
them front-to-back for `SALE_FIFO`. Positions are **never sorted** (load
order / insertion order, see `portfolioLoad`/`portfolioAdd`). If a user adds
an older lot after a newer one, "FIFO" sells the newer lot first, changing
cost basis and realised P/L vs. the intuitive "first in, first out".

Fix options (not applied): sort lots by `date` (then id) before allocation,
or rename the method "in portfolio order" in UI/docs.

## D. LOW — findings with limited blast radius

1. **CSV `lots` column can silently truncate.** `csvLine()` builds the lots
   list into `char lots[256]`; with `MAX_LOT_ALLOCS=12` at `%.8f@%.6f`
   precision a full record needs ≈380 B. `snprintf` overflow just `break`s —
   no flag, no note in the header. Data-loss-ish only for exports of
   8+-lot sales with long decimals.
2. **Reversals are dated to the original sale date** (`makeReversal` copies
   `orig.date`), so `yearAdd` books a reversal into the *sale's* year, not
   the year the undo happened. This is a defensible policy but the web
   `years` array and the LCD THIS YEAR card never say so.
3. **`yearPl()` returns 0 for years not in the bucket list**, and buckets
   are `YEAR_BUCKETS=4`. Once the ledger spans 5+ years, old years vanish
   from `/api/realised` `years[]` while `yearPl` "known: no sale in this
   year" returns a confident 0 for a year that actually had sales (current
   year can't be evicted, so the LCD card stays correct).
4. **`POST /api/sales/reverse {"seq":0}` reverses the newest sale**
   (FindCtx treats seq 0 as "latest"). Validation allows `seq >= 0`; likely
   harmless/undocumented feature, but a caller passing a truncated/parsed 0
   would undo the wrong record.
5. **Unauthenticated GETs expose portfolio numbers** (NAV, P&L, positions,
   settings incl. hostname), with `Access-Control-Allow-Origin: *`. By
   design for the dashboard, but it is financial data readable by any device
   on the LAN (and any web page the user visits could read it via fetch —
   Basic-auth creds are not attached cross-site, but with no password set
   there is nothing to attach).

---

## E. Robustness — genuinely strong (verified, not assumed)

- **Ledger integrity**: tmp→rename atomic writes everywhere; pre-append
  line-length guard (`LEDGER_LINE_MAX`); bad lines quarantined at boot
  (`sal009`, `badLines`) instead of crashing the parser; crash-recovery
  journal written *before* portfolio mutation and replayed idempotently
  (seq dedup via `findSeq`); compaction runs before the ledger can hold a
  line that would overflow the parser (`LEDGER_HARD_LIMIT = 0.8 ×
  LEDGER_LIMIT` — verified 1600 < 2000 in `config.h`).
- **Parser bounds**: `JsonStream` caps value length per key (e.g. symbol 16
  → dest 20, date 10 → dest 12) and depth 3; `FlatJson` truncates into
  caller buffers. No raw `strcpy` from network input into fixed buffers
  (only `strcpy(p.symbol, sym)` where `sym` is already normalized/bounded).
- **Heap discipline**: one market job at a time; 16 KB floor before each
  TLS connect (live: free_min 150 KB); body read in 384 B slices; chart
  buffers malloc'd per request and freed on every early-return path
  (checked the `dt||dH||dL||dC` partial-failure free path); XPT2046
  calibration table dropped under 8 KB.
- **Recovery paths**: boot repairs (ledger→ledger.bad, tmp orphan sweep,
  journal replay) are covered by unit tests; captive portal +
  `ensureApFallback()`; watchdog untouched, loop yields every 2 ms.
- **Test/verification culture**: 128 host unit tests, 22 web-API scenario
  tests, and a pixel-exact preview harness that checks every UI slot for
  out-of-bounds ink and slot-vs-slot overlap on 76 frames — currently
  0 problems, including the 1.1.9 Realised rebalance.

## F. Cosmetic / maintainability (no action taken)

- `DisplayUI.cpp` `inkOf()` entry for `F1` is `{10,3}` but the measured
  value from the preview font dump is `{7,0}` (8x8 bitmap, 7-pixel ink).
  Harmless today because no baseline-centred slot uses F1 (the last-sale
  strip now hard-codes +3/+18 offsets), but the stale table row will
  mislead the next layout change.
- `OTAManager.cpp`: unused `constexpr MIN_FREE_BLOCK` (superseded by
  `minFreeBlockBytes()`); `WebServerManager::sendErr` has dead
  `char b[120]` + `(void)b`.
- `secrets.example.h` asks for `OTA_PASS` which nothing reads (see A).
- README line 170 documents "ArduinoOTA … password = admin password" —
  correct, but pairs confusingly with the unused `OTA_PASS`.
- `tests/test_core`, `tests/test_sales` build artifacts were untracked
  noise; now covered by `.gitignore` (done as part of the 1.1.9 commit,
  build outputs only).

## G. Suggested priority order (for a future session, nothing applied)

1. Set an admin password on the device, or gate `/update` + ArduinoOTA on a
   non-empty password; delete or wire up `OTA_PASS`. (High, config-only fix
   available today.)
2. Decide FIFO semantics: sort lots by date before allocation, or rename.
   (Medium, accounting correctness.)
3. Add a code comment (or Kconfig) for `setInsecure()`; consider
   `setCACert`. (Medium, threat-model honesty.)
4. Widen `csvLine`'s `lots` buffer to ~384 B or emit a `lotsTruncated`
   flag. (Low.)
5. Document/flag reversal dating and 4-year bucket window in web UI. (Low.)
6. Tidy F block (dead constants, inkOf row, secrets example). (Low.)

## H. 1.1.9 diff-specific review (this session's changes)

- `R_LAST_Y 180→174`, `R_CARDS_Y 144→138`, big-number/THIS YEAR shifted:
  consistent; `R_SALE_W`/`R_CARDS_W` untouched; touch zones
  (`TouchZones::kRows`) do not depend on these constants.
- `FG18` for SALES/REVERSED counters + `+4` offset inside 30 px cards:
  preview shows digits with ~6–8 px head/foot clearance, 0 ink overlaps.
- `F1` last-sale rows at `+3`/`+18` with `R_LAST_Y=174`: ink rows now
  177–184 and 192–199; 7 px gap (was 1 px at 1.1.8), divider at 171 is
  5 px above first ink — clean. `R_LAST_Y-3` static divider and dynamic
  clear rect (`y-1`, `h+2`) still don't touch the divider.
