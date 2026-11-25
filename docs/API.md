# HTTP API

Base URL: `http://<device-ip>/` or `http://sp500-32.local/`. All API bodies are JSON (`Content-Type: application/json`).
All responses are compact JSON. Money is THB (portfolio) or index points (S&P 500); the two are never mixed.

Conventions

* `null` means **not available** (never `0`). Clients must render `N/A`.
* `*Epoch`, `t[]`, `updated` are Unix seconds (UTC). `0` = never.
* `stale: true` = last refresh failed or is older than 3x the refresh interval (cached value is still shown).
* Errors: `{"ok":false,"error":"text"}` with 4xx/5xx. Unknown `/api/*` -> 404 same shape.
* `GET /api/*` carry `Access-Control-Allow-Origin: *`. Mutating calls are same-origin only.
* If an admin password is set (Settings), every `POST` and `/update` need HTTP Basic auth (`admin:<password>`), and so do all sale endpoints (`/api/realised`, `/api/sales*`, `/api/lots`) because they expose money; a 401 carries `WWW-Authenticate: Basic`.
* Passwords (Wi-Fi, admin) are write-only. They are never returned.

## Read endpoints

### GET /api/status
```json
{"fw":"1.1.4","uptime":3600,
 "time":{"epoch":1795210554,"synced":true},
 "heap":{"free":21000,"maxBlock":17000,"frag":12},
 "chip":{"id":"A1B2C3","cpuMHz":160,"flashKB":4096,"sketchKB":512,"freeSketchKB":1019,"sdk":"3.1.2"},
 "fs":{"ok":true,"totalKB":1024,"usedKB":16},
 "wifi":{"mode":"sta","connected":true,"ssid":"home","ip":"192.168.1.50","rssi":-61,"mdns":"sp500-32.local"},
 "data":{"lastMarketOk":1795210560,"lastError":"HTTP 429","lastErrorEpoch":1795210000,"quoteStale":false,"portfolioStale":false,"blocked":""},
 "tls":{"verified":true,"clock":"synced","hsMs":1631,"heapBefore":207912,"heapAfter":159876,"blockBefore":110580,"blockAfter":90100,"bundle":2254},
 "certs":{"source":"builtin","roots":3,"hash":3623160014},
 "ota":{"active":false}}
```
`wifi.mode`: `sta` | `ap-setup` (no saved network) | `ap-rescue` (saved network unreachable). `lastError` is `""` when none. `data.blocked` explains why the market scheduler is idle (`"clock not synced"` until NTP completes - TLS needs a valid clock to check certificate time windows). `tls` describes the last HTTPS handshake: `verified` is always true on fw >= 1.3.0 (unverified requests cannot happen), `hsMs` handshake time, heap/min-block before/after the TLS session, `bundle` bytes fetched. `certs` = same object as `GET /api/certs`.

### GET /api/portfolio
```json
{"currency":"THB","updated":1795210560,
 "summary":{"invested":250000.00,"nav":268500.00,"pl":18500.00,"returnPct":7.40,
            "realisedPl":-3910.00,"lifetimePl":14590.00,
            "positions":3,"priced":3,"invalid":0,"complete":true,"stale":false}}
```
`nav`, `pl`, `returnPct` are `null` unless `complete` (every valid position has a quote). `returnPct` is `null` when invested = 0.

`realisedPl` = all-time realised P/L from `/sells.jsonl` (`null` while the ledger has not been read). `lifetimePl` = `pl + realisedPl`, `null` while either side is unknown (never `0` for "unknown"). The open-position P/L is labelled **Unrealised P/L** everywhere in the UI.

### GET /api/positions
```json
{"currency":"THB","summary":{...same as above...},
 "positions":[{"i":0,"id":1,"symbol":"^GSPC","date":"2025-11-20","qty":39.170000,"oqty":39.170000,
   "buy":6382.100000,"price":6910.39,"cost":250000.00,"nav":270690.05,"pl":20690.05,"returnPct":8.28,
   "quote":"fresh","quoteEpoch":1795210554}]}
```
`quote`: `fresh` | `stale` | `missing` | `invalid` (bad qty/buy -> excluded from totals). `price`, `nav`, `pl`, `returnPct` are `null` if `missing`/`invalid`. `quoteEpoch` is the market time of the price.

`id` is the **lot id** (unique, never reused) and `oqty` the quantity at purchase; `qty` is what is still open. A sale reduces `qty` and disappears from this list once the lot is fully sold (its history stays in `/api/sales`; the per-lot view is `/api/lots`).

### GET /api/market
```json
{"symbol":"^GSPC","valid":true,"stale":false,
 "last":6910.39,"prevClose":7175.83,"change":-265.44,"changePct":-3.70,
 "open":7150.39,"high":7198.67,"low":6882.66,
 "status":"CLOSED","sessionDate":"2026-11-20",
 "quoteTime":1795210554,"quoteTimeText":"Nov 20, 16:35 EST","timezone":"EST","tzName":"America/New_York",
 "fetchedEpoch":1795210560}
```
`status`: `OPEN` | `CLOSED` | `PRE-MARKET` | `N/A` (clock not yet synced). `quoteTimeText` honours the Settings timezone mode. Numbers that are unknown are `null`. When `valid` is false everything except `symbol`/`status` is `null`.

### GET /api/chart?range=1d|5d|1mo
```json
{"range":"1d","interval":"5m","valid":true,"stale":false,"updated":1795210560,"count":79,
 "first":7150.39,"last":6910.39,"base":7175.83,"change":-265.44,"changePct":-3.70,
 "min":6882.66,"max":7198.67,
 "t":[1795185000,1795185300],"v":[7150.39,7135.65],
 "o":[7150.39,7140.10],"h":[7155.02,7142.30],"l":[7148.11,7130.04]}
```
`base` is the reference for `change`: previous close for `1d`, first point for `5d`/`1mo`. `t[]`/`v[]` are parallel arrays, no synthetic points. `v` is the close; `o`/`h`/`l` are the matching open/high/low of every bar (candlesticks) - same length as `v`, and `min`/`max` already include the wick extremes. A bar Yahoo did not supply a full candle for is a doji at the close (`o=h=l=v`), never an invented range. Optional `&max=N` (N>=8) aggregates: first and last bar verbatim, each middle bucket merged into one candle (open = first open, high = bucket max, low = bucket min, close = last close). If never fetched: `valid:false,count:0,t:[],v:[],o:[],h:[],l:[]`. Bad `range` -> 400.

### GET /api/wifi/status
```json
{"mode":"sta","state":"connected","ssid":"home","ip":"192.168.1.50","rssi":-61,"mdns":"sp500-32.local",
 "saved":true,"apSsid":"","apIp":"",
 "connect":{"state":"idle","ssid":"","error":""},
 "scan":{"state":"idle","ageSec":12,"count":7}}
```
`state`: `connected` | `connecting` | `disconnected`. `connect.state`: `idle` | `connecting` | `ok` | `failed` (`error` set on `failed`). `scan.state`: `idle` | `scanning` | `done`. `apSsid`/`apIp` only while an AP is up (`rescue` AP key is shown on the LCD only).

### GET /api/wifi/scan[?refresh=1]
Non-blocking. Starts a scan when none is cached/running (or `refresh=1`), returns immediately.
```json
{"scanning":false,"ageSec":3,"networks":[{"ssid":"home","rssi":-52,"enc":true,"ch":6}]}
```
While `scanning` the previous list (possibly empty) is returned. Poll every ~1.5 s.

### GET /api/settings
```json
{"portfolioRefreshS":60,"quoteRefreshS":60,"chartRefreshS":300,"pageRotateS":8,"backlight":100,
 "bbLcd":0,"bbLcdP":20,"defaultRange":"1d","tzMode":"exchange","localOffsetMin":420,"hostname":"sp500","adminPassSet":false,
 "limits":{"portfolioRefreshS":[15,3600],"quoteRefreshS":[15,3600],"chartRefreshS":[60,3600],
           "pageRotateS":[3,120],"localOffsetMin":[-720,840],"backlight":[5,100],"bbLcd":[0,1],"bbLcdP":[10,50]}}
```
`bbLcd` (`0`|`1`, default `0`) and `bbLcdP` (**10, 20 or 50 only** - the `limits` range is [10,50] but any other value is rejected with `bbLcdP must be 10, 20 or 50`) drive the panel's Market chart: on, that chart is drawn as candlesticks (the same `o`/`h`/`l`/`v` the web chart gets) with **Bollinger Bands** - SMA(`bbLcdP`) plus/minus a fixed 2 sigma - over it; off, the chart stays the single-colour line and fill it always was. The panel clamps the period to half its stored bars (1M keeps only 24 daily bars), needs at least 6 bars, and prints the period actually used in the corner of the plot, so the screen never claims a band it did not draw. Bands are computed from closes; a series with no wick at all (`h == l`) keeps the line even when the switch is on. Changing either value repaints the chart page immediately (`POST /api/settings` -> chart redraw, no full-screen flash). Only sigma is fixed: the web dashboard keeps its own period/width controls, which are per browser (`localStorage`) and independent of `bbLcd`.

`pages` (`1..15`) and `charts` (`1..7`): bitmasks of the LCD pages / Market chart ranges that rotate (see the SmallTV docs). On the CYD they steer the automatic rotation: a screen whose `pages` bit is 0 is skipped (the bit values are historical and fixed: Portfolio=1, Positions=2, Chart=4, Realised=8 - the checkbox numbers in the web UI show the LCD position instead), and each time the rotation enters the chart screen it steps to the next enabled `charts` range (1D=1, 5D=2, 1M=4). Touching a nav zone or a period button always works, so the masks can never lock you out of a screen. The LCD nav runs left to right **Dashboard | Chart | Positions | Realised P/L** (nav zone id == screen id; the `pages` bits keep their historical numbering, so a stored `config.json` still means the same pages). `defaultRange`: `1d`|`5d`|`1mo`. `tzMode`: `exchange` (America/New_York) | `local` (fixed UTC offset `localOffsetMin`, no DST). Bangkok is `local` + `420` (ICT has never observed DST, so a fixed offset is exact); the web UI's **Zone preset** dropdown offers Bangkok, UTC+8, Tokyo/Seoul, India and UTC and fills the offset for you. In `local` mode the zone shown by `/api/market` is `UTC+07:00` while `tzName` stays the instrument's exchange zone - NYSE session hours and the OPEN/CLOSED badge always follow America/New_York rules, never the display zone.

`backlight` (`5..100`, added in fw 1.1.5): LCD brightness in percent. POST it on its own and the panel dims straight away - no restart, and the **LCD brightness (%)** slider on the Settings tab does exactly that when you let go of it. The backlight pin is driven by 5 kHz / 10-bit PWM, and the slider position is squared onto duty because an LED string's light output is linear in duty while the eye is not: 50 % is about a quarter of the light, 75 % a little over half. 5 % is a hard floor (2.3 % duty) - the screen can never be dimmed past the point where only the web UI could bring it back, and the bottom of the slider always changes something. The level is kept in `settings.json`, so it survives reboots and OTA updates; a settings file written before 1.1.5 simply gets 100.

### GET /api/positions/job
State of the last "add position" request: `{"state":"idle|pending|ok|error","error":"","symbol":"^GSPC","updated":false}`.

### GET /api/realised
Aggregates over the whole sale ledger (admin auth). One streaming pass at boot, then kept in RAM.
```json
{"valid":true,"pl":-3910.00,"lifetimePl":14590.00,
 "count":3,"reversed":1,"records":4,
 "limit":2000,"warn":false,"full":false,"recovery":false,"badLine":false,
 "hasLast":true,"lastId":"TX-000003","lastSymbol":"^GSPC","lastDate":"2026-11-19",
 "lastRealised":-3910.00,"lastTs":1795210560,
 "years":[{"year":2026,"pl":-3910.00},{"year":2025,"pl":1200.00}]}
```
`count` = sales that still stand (reversed ones excluded), `reversed` = number of undo records, `records` = all lines in the ledger. `pl` is the sum over valid **and** reversal records (a reversal carries the negated amount). `years` holds up to 8 newest calendar years, newest first. `valid:false` means the ledger could not be read -> `pl` is `null` (never `0`). `warn`/`full` flag the 2000-line ledger cap (warning from 1800). `recovery`/`badLine` report boot-time repair.

### GET /api/sales?symbol=^GSPC&offset=0&limit=8
One page of the ledger, newest first (admin auth). `limit` is clamped to 1..8.
```json
{"symbol":"^GSPC","total":4,"offset":0,"count":2,"limit":8,
 "records":4,"limitRecords":2000,"warn":false,"full":false,"pl":-3910.00,
 "sales":[{"seq":4,"ts":1795210560,"id":"TX-000004","refId":"","type":0,"status":"valid",
   "symbol":"^GSPC","date":"2026-11-19","qty":10.00000000,"price":6200.000000,"refPrice":6200.000000,
   "priceSource":"yahoo","fee":10.0000,"gross":62000.000000,"net":61990.000000,
   "cost":63821.000000,"realised":-1831.000000}]}
```
`type`: `0` sale, `1` reversal (undo). `status`: `valid` | `reversed` (this sale was undone) | `reversal`. `refId` is set on reversals (id of the original sale). `priceSource`: `manual` | `yahoo`. Rows never carry their lot allocations; the lot view of the same records is `GET /api/lots` below.

### GET /api/lots?symbol=NVDA
Per-lot ledger history for one symbol (admin auth, fw 1.3.3+). Read-only: one streaming pass over the ledger, no flash writes.
```json
{"ok":true,"symbol":"^GSPC","ledger":true,"badLines":0,
 "lots":[{"id":1,"nSales":2,"take":20.00000000,"capped":false,
   "refs":[{"seq":3,"id":"TX-000003","date":"2026-11-18","type":"sale","take":12.00000000,
            "before":67.41642896,"after":55.41642896,"realised":-1831.0},
           {"seq":5,"id":"TX-000005","date":"2026-11-20","type":"reversal","take":-12.00000000,
            "before":55.41642896,"after":67.41642896,"realised":null}]}]}
```
Only currently open lots are listed. `take` is the **net** quantity the ledger moved on the lot: a sale's allocation is `before-after` (positive) and its reversal re-runs the lot with the negated amount, so an undone sale cancels itself out automatically. `nSales` counts sale records only (reversals not counted). Up to the **12 most recent** ledger references per lot are kept in `refs`, oldest first; `capped:true` means `nSales` exceeds what is shown. `realised` is the sale-level figure and is deliberately not split across lots - a sale spanning several lots has exactly one realised amount (`null` on reversals). `ledger:false` means the ledger file could not be opened: the lot list is still correct but every history field reads as empty (never guess). `badLines` counts unparseable ledger lines.

### GET /api/sales/export[?symbol=^GSPC]
`text/csv` attachment (`sells.csv`), streamed. Header:
`seq,date,id,type,status,symbol,qty,price,fee,gross,net,cost,realised`

### GET /api/sales/job
State of the last "preview a sale" price lookup: `{"state":"idle|pending|ok|error","symbol":"^GSPC","date":"2026-11-19","price":6200,"error":""}`.

Notes the dashboard relies on

* `positions[].cost` is `null` for `invalid` rows (bad qty/buy); they are excluded from every total.
* `/api/portfolio` `updated: 0` = no quote refreshed yet.
* `/api/positions/job` reports `ok`/`error` once, then returns to `idle` (the result is consumed by the read).
* After `POST /api/wifi/connect`, `connect.state` stays `ok`/`failed` until the next connect, so a page that lost the connection can still read the result.
* The dashboard shows and accepts dates as dd/mm/yyyy; this API always uses ISO `yyyy-mm-dd`.
* `realisedPl`/`lifetimePl` ride `/api/portfolio` (the page that already polls) so the header cards stay cheap; the sale endpoints need admin auth and are only called on the `#sales` page.

### GET /api/certs
```json
{"ok":true,"certs":{"source":"builtin","roots":3,"hash":3623160014}}
```
Which root CA bundle verifies every Yahoo TLS session right now: `builtin` (firmware, PROGMEM) or `custom` (`/ca/bundle.pem` on LittleFS, installed via the API below). `hash` is an FNV-1a checksum of the active PEM - use it to confirm what the device actually trusts after an install. See [docs/CERTS.md](CERTS.md).

## Write endpoints

| Method + path | Body | Success | Notes |
|---|---|---|---|
| `POST /api/settings` | any subset of the settings fields; `adminPass` (string, "" = unchanged), `clearAdminPass:true` | `200 {"ok":true,"settings":{...as GET...}}` | all-or-nothing validation, `400 {"ok":false,"error":"quoteRefreshS must be 15..3600"}`. Changing `hostname` takes effect after restart (`"restart":true` in the reply). |
| `POST /api/refresh` | `{"what":"all"\|"quote"\|"charts"\|"portfolio"}` | `202 {"ok":true}` | queues an immediate refresh |
| `POST /api/cache/clear` | `{}` | `200 {"ok":true}` | drops cached quotes/charts (RAM + flash) and refetches |
| `POST /api/positions` | `{"symbol":"^GSPC","date":"YYYY-MM-DD","thb":10000}` **or** `{"symbol","date","qty","buy"}` | `202 {"ok":true,"state":"pending"}` (thb form, poll `/api/positions/job`) or `200 {"ok":true,"state":"ok"}` (qty/buy form) | `thb` form: buy price = close on that date, `qty = thb / buy`. `409` if a job is pending. Same symbol+date updates the existing row. Max 12 positions. |
| `POST /api/positions/delete` | `{"index":0}` | `200 {"ok":true}` | |
| `POST /api/sales/preview` | `{"symbol":"^GSPC","date":"YYYY-MM-DD","method":"fifo"\|"specific","mode":"qty"\|"thb"\|"all","lotId":1,"qty":10,"amount":10000,"price":6200,"fee":10}` | `200 {"ok":true,"state":"ready","preview":{...}}` | Validates + allocates the lots and computes the money **without writing anything**. Without `price` it answers `202 {"ok":true,"state":"pending"}` and looks up the closing price of `date` (poll `/api/sales/job`, then re-post the same body; the reply is then `ready` and `priceSource:"yahoo"`). `400` with a readable reason (`"only 51.67 available"`, `"no open position for that symbol"`, `"sale date is in the future"`, `"choose a lot"`, ...). |
| `POST /api/sales/confirm` | the preview body + `"token":<preview.token>` | `200 {"ok":true,"state":"committed","id":"TX-000004","seq":4,"preview":{...}}` | The token binds the commit to the portfolio state the preview was computed from: any lot change, new sale or undo in between gives `409` (`"preview is out of date..."`). Never deletes anything; the lot quantities are set to the preview's `after`. |
| `POST /api/sales/reverse` | `{"seq":4}` | `200 {"ok":true}` | Appends a reversal record (an undo is a new line, never an edit). `400` for an unknown `seq`, an already reversed sale, or a reversal of a reversal. |
| `POST /api/sales/delete` | `{"seq":4}` | `200 {"ok":true}` | **Permanently** removes one transaction from `/sells.jsonl`. A sale and its reversal are always removed together (`seq` may name either); an active sale is reversed first so the position is restored. Later records are renumbered (`TX-` ids are kept). `404` unknown `seq`, `409` ledger has unreadable lines. Rewrites the file through a temp copy, finished at boot if power is cut. |
| `POST /api/certs/install` | raw PEM bundle as `text/plain` body (1..8 certificates, <= 8 KB) | `200 {"ok":true,"certs":{...}}` | **Live-verified activation**: the device performs a real TLS fetch of Yahoo with the candidate roots; only on success is `/ca/bundle.pem` swapped in (atomic rename). Any failure (`400` malformed PEM, `413` too large, or `{"ok":false,"error":"TLS refused by the new roots (ssl -9984: ...)"}`) leaves the previous bundle active. Recover from a Yahoo CA change without a reflash. |
| `POST /api/certs/reset` | `{}` | `200 {"ok":true,"certs":{"source":"builtin",...}}` | deletes the custom bundle, falls back to the firmware bundle |
| `POST /api/wifi/connect` | `{"ssid":"home","password":"..."}` | `202 {"ok":true,"state":"connecting"}` | non-blocking; poll `/api/wifi/status` -> `connect.state`. Credentials are saved only after a successful join; on failure the old network is restored. |
| `POST /api/wifi/reconnect` | `{}` | `200 {"ok":true}` | |
| `POST /api/wifi/forget` | `{"confirm":"FORGET"}` | `200 {"ok":true}` | deletes saved credentials, device starts the setup AP |
| `POST /api/system/restart` | `{}` | `200 {"ok":true}` | restarts ~1 s later |
| `POST /api/system/reset-settings` | `{"confirm":"RESET"}` | `200 {"ok":true}` | defaults for settings only (Wi-Fi + portfolio kept), restarts |
| `POST /api/system/factory-reset` | `{"confirm":"FACTORY"}` | `200 {"ok":true}` | erases settings, Wi-Fi, portfolio, cache; restarts into the setup AP |
| `POST /update` | multipart form, file field `firmware` (`.bin`) | `200 text "OK, rebooting"` | on failure `500 text "update failed: ..."` |

Symbols are upper-case `[A-Z0-9^.=-]`, 1..15 chars. Aliases `SP500`, `S&P500`, `GSPC` -> `^GSPC`; `SPX` -> `^SPX`; `NDX`, `NASDAQ` -> `^NDX`.

Sale-integrity note (fw >= 1.3.0): on `sales/confirm` with `priceSource:"yahoo"`, the device re-checks its **own** verified close for that symbol+date. The stored `refPrice` is the device's value, never the client's; an execution price more than 5% away from it gives `409` (`"... moved a lot since preview - preview again"` / `"the device's Yahoo close is no longer fresh - preview again"`). Money fields are bounded (price < 1e10, fee < 1e12, amount < 1e15, qty < 1e12) so every figure round-trips exactly through float->decimal->float.

## Sale ledger

A sale is **Input -> Preview -> Confirm -> Commit**, never a single mutating call:

1. `POST /api/sales/preview` returns the exact allocation and the money:
```json
"preview":{"symbol":"^GSPC","date":"2026-11-19","price":6200.000000,"refPrice":6200.000000,"priceSource":"yahoo",
 "qty":10.00000000,"available":51.67000000,"fee":10.0000,
 "gross":62000.000000,"net":61990.000000,"cost":63821.000000,"realised":-1831.000000,"token":2461770063,
 "alloc":[{"lot":1,"date":"2025-11-20","buy":6382.100000,"oqty":39.17000000,"before":39.17000000,"after":29.17000000}]}
```
`qty` is what is actually taken (`mode:"thb"` divides by the price), `available` the open quantity of that symbol before the sale. `cost` = `Σ(before - after) x buy` of the touched lots (FIFO = oldest purchase date first; `specific` = one lot by `lotId`), `net = gross - fee`, `realised = net - cost`. `alloc[].after` is the lot's open quantity after the sale.

2. The client shows the preview, then `POST /api/sales/confirm` with the `token`. The commit applies the allocation, drops fully sold lots, bumps the portfolio `ver`/`appliedSeq` and appends one line to `/sells.jsonl`:
```json
{"seq":4,"ts":1795210560,"id":"TX-000004","type":0,"symbol":"^GSPC","date":"2026-11-19",
 "qty":10.00000000,"price":6200.000000,"refPrice":6200.000000,"priceSource":"yahoo",
 "fee":10.0000,"gross":62000.000000,"net":61990.000000,"cost":63821.000000,"realised":-1831.000000,
 "alloc":[{"lot":1,"date":"2025-11-20","buy":6382.100000,"oqty":39.17000000,"before":39.17000000,"after":29.17000000}]}
```
3. `POST /api/sales/reverse` appends `{"type":1,"refSeq":4,"refId":"TX-000004",...,"realised":1831.000000}` and gives the quantity back to the lots (a sold-out lot is re-created).

Guarantees

* Sales and undos are **append-only** records: an undo is a new record, never an edit. The one exception is the explicit `POST /api/sales/delete`, which removes a sale and its reversal for good.
* Applying a record sets `qty = alloc.after` (absolute), so replaying it twice is a no-op.
* Commit order is journal -> append -> save portfolio; `/txn.pending` holds the in-flight record so a power cut mid-append is completed (or the partial tail truncated) at the next boot. `/portfolio.json` `appliedSeq` marks what is already reflected.
* `version` 2 of `/portfolio.json` adds `ver`, `appliedSeq`, `nextLotId`, `nextTxn` and per-lot `id`/`oqty`; a v1 file loads unchanged and is migrated on the next write.
* Ledger lines are capped at 2000 (`warn` at 1800, then `full` and the UI blocks new sales).

## Web pages

`GET /` serves the single-page dashboard (gzip). Hash routes: `#portfolio` (default), `#market`, `#holdings`, `#sales`, `#wifi`, `#settings`, `#system`. In AP mode any unknown host/URL is redirected to `http://192.168.4.1/#wifi` (captive portal).
