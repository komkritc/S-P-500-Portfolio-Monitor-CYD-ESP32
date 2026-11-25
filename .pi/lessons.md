# Lessons - ESP32-CYD-SP500

Facts re-verified on 2026-11-25 with a firmware build, unit tests, the preview harness
and live requests to the device. Do not trust memory over them.

## Two devices, one LAN - do not mix them up

| Host | IP | Board | Repo |
|---|---|---|---|
| `sp500-32.local` | 192.168.1.161 | ESP32 CYD (this repo) | `ESP32-CYD-SP500` |
| `sp500.local` | 192.168.1.187 | ESP8266 SmallTV (GeekMagic, ST7789 240x240) | `SP500_SmallTV/` in the **parent** repo `/data/Projects/sp500` (see below) |

Before flashing, confirm `fw`, `chip.id` (CYD = `F3AF48`) and uptime in `/api/status`.
The CYD runs Arduino/ESP32 core 2.0.17, ESP8266 core 3.1.0 belongs to the other board.

## Two ways to push firmware to the CYD - both verified

1. **`pio run -e cyd_ota -t upload --upload-port sp500-32.local`** (espota).
   ArduinoOTA on ESP32 core 2.0.17 uses **UDP** 3232: a TCP probe of that port always
   says "Connection refused" even when the updater is alive, and `espota.py` answers the
   challenge in UDP. A failed TCP probe proves nothing.
2. **`curl -F firmware=@.pio/build/cyd_ota/firmware.bin http://sp500-32.local/update`** -
   the ESP32 `/update` page posts the field name `firmware`. No auth while `adminPassSet:false`.
   Reboot takes ~22 s, then `/api/status` answers.

`/api/status` `fw` is the firmware version (`FW_VERSION` in `src/config/AppConfig.cpp`) -
bump it per release. Do not reuse a version another board already reports (1.1.3 = SmallTV).
No git tag unless asked. `curl`/`pio` need the network unlocked by the user; only `sp500-32.local`
name-resolves - address the CYD directly by 192.168.1.161 if mDNS fails.

## Building and testing (all verified)

* `pio run -e cyd` (or `cyd_ota`) needs `include/secrets.h`; copy it from
  `include/secrets.example.h` first and delete it when done (gitignored) - if it stays,
  the tree is dirty. Flash ~76 %, RAM ~20 %.
* `web/index.html` is compiled into the firmware: after editing it, run
  `python3 tools/gen_web_assets.py` (gz limit 22,528 B) before `pio run`. fw 1.1.7 (candlesticks
  + Bollinger Bands) uses **21,581 B gz - only ~950 B of headroom left**, so any new web feature
  has to be measured with this tool first; shrinking means trimming the minified markup, not the
  compressor (already level 9). From here only the served bytes are checkable
  (`curl http://sp500-32.local/ | gunzip` and diff against the minified file) - what the browser
  actually draws needs a real browser.
* `python3 tests/run_tests.py` builds host unit tests from `src/` (1289 asserts, plus 128 in the
  sale-ledger binary); `DisplayUI.cpp` is not in that file list, so the host build never compiles
  it. It also leaves untracked ELF binaries `tests/test_core` and `tests/test_sales` behind - they
  are **not** gitignored, delete them before committing.
  `python3 tests/preview/run_preview.py` renders 71 LCD frames with a mock TFT; it compiles
  `DisplayUI.cpp` and calls the parser/chart store itself, so any change to those signatures
  must be mirrored in `tests/preview/preview.cpp` or the preview build breaks.
* Host tests link `src/` against mocks in `tests/` and compile with plain g++: any ESP32
  API used in a compiled file must be guarded (`#if defined(ESP32)`), and new
  `DisplayUI.cpp` code must compile against `tests/preview/mock/TFT_eSPI.h`.
* ESP32 core here is **2.0.17**: `ledcSetup`/`ledcAttachPin` exist, the core 3.x
  `ledcAttach(pin, freq, res)` does not. `TFT_eSPI` 2.5.43 has no
  `setBacklightBrightness()` - it only switches `TFT_BL` on; PWM is ours to add.

## Two hardware-only checks the assistant cannot do remotely

1. **Touch** (`src/TouchUI.cpp`) - XPT2046 calibration drift needs finger taps, and the
   panel's actual response has to be seen.
2. **Backlight / anything judged by brightness on the panel** (`UI::setBacklight`).
   The API confirms the number and the duty maths, only the eye judges the result.
   Report these as needing physical verification; never claim them as verified.

## The two repositories (verified 2026-11-25)

| Directory | Git | Holds |
|---|---|---|
| `/data/Projects/sp500` | `.git` at that root, branch `main` | **both** project folders: `SP500_SmallTV/**` (live) and an old copy of `ESP32-CYD-SP500/**` from before the split. Remote `gh` = `github.com/komkritc/S-P-500-Portfolio-Monitor-SmallTV-ESP8266-`; branch **`publish-smalltv`** (tracks `gh/main`) is that repo's shape: its root *is* the project, no `SP500_SmallTV/` prefix |
| `/data/Projects/sp500/ESP32-CYD-SP500` | its own `.git` (nested) | the CYD firmware only. Remote `origin` = `github.com/komkritc/S-P-500-Portfolio-Monitor-CYD-ESP32` |

Never `git checkout publish-smalltv` / `smalltv-only` **inside** `/data/Projects/sp500`:
that tree has no `ESP32-CYD-SP500/` path, so the checkout would delete the CYD folder
from the worktree. Use a worktree under /tmp (recipe below).

### Publishing the SmallTV tree (used for fw 1.1.6, 2026-11-25)

```bash
git -C /data/Projects/sp500 fetch gh                       # gh/main may have GitHub-only commits
git -C /data/Projects/sp500 worktree add -b publish-smalltv /tmp/stgh gh/main
cd /tmp/stgh && find . -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
git -C /data/Projects/sp500 archive <commit> -- SP500_SmallTV | tar -x --strip-components=1 -C /tmp/stgh
git add -A && git commit          # once per release step (the archive strips the folder prefix)
git -C /data/Projects/sp500 push gh publish-smalltv:main
git -C /data/Projects/sp500 worktree remove /tmp/stgh
```

Verify the published tree matches: archive the same commit to a second temp dir and
`cmp` every `git ls-files` path. `gh auth status` claims the token is invalid, yet
`git push` authenticated fine on 2026-11-25 (both repos) - try the push before
re-authenticating.

The CYD folder's own repo wins inside it, so `git status` there is clean while the same
files show as modified from the parent. **Never `git add -A` in the parent** - it would
commit the stale CYD copy. Commit there with explicit pathspecs (`git add SP500_SmallTV`).
Branch `smalltv-only` / remote `gh/main` is a history whose root *is* the SmallTV project.

## ESP8266 SmallTV facts (fw 1.1.6, verified live on .187)

* Backlight: ST7789 wired **active-low** on GPIO 5 (D1), `analogWrite` with
  `analogWriteRange(255)` - duty 0 = full on, 255 dark. `lcdBegin()` lights it before
  anything else boots (a dark screen there means the sketch never got that far), then
  `setup()` applies the stored level once `settings.json` is readable.
  Default `90` %, the level this board shipped with; the CYD's default is 100 %.
* ESP32 CYD is active-**high** LEDC, 5 kHz / 10 bit. Both use the same perceptual curve
  (slider squared onto duty); only the polarity and the range differ.
* OTA: `pio run -e smalltv -t upload --upload-port sp500.local` (ArduinoOTA on **8266**)
  took 11 s for 715 KB. An HTTP `POST /update` (multipart field `firmware`) also exists.
  The updater needs `ESP.getMaxFreeBlockSize() >= 6000`; this device runs with 10-20 KB
  blocks, so heap matters: no new static buffers, keep JSON parsing on the heap.
* Firmware numbering is shared across the two boards: 1.1.3 belongs to SmallTV, 1.1.4 and
  1.1.5 to the CYD, so the SmallTV dim release is **1.1.6**. Never let both devices report
  the same `fw`.

## Touch input facts (fw 1.1.4)

* A gesture must end at the **last point under the finger**, not at whatever `readPt()`
  left in `x,y`: the driver returns 0,0 once a touch is released, so ending at `x,y` turns
  every tap into a full-screen swipe.
* Keep relative actions (swipe -1/+1) in a **separate enum** from absolute screen ids
  (0..3). One shared "action id" silently collides - nav zone 1 is Chart, swipe-forward is
  also 1, so a swipe from Positions landed on Dashboard and one from Realised did nothing.
* `Touch::begin()` calls `ts.setTouch(1)` after every `tft.init()` (ILI9341 init pulses the
  HSYNC/VSPI lines XPT2046 shares) and re-probes the controller in `loop()` if the boot probe
  failed, so a touch timeout never blocks the web server.

## Panel chart facts (fw 1.1.8 - Bollinger Bands + candlesticks)

* `py()` inverts the axis (a higher price is a **smaller** y). Derive a candle body from the
  prices, never from the direction flag: `y1 = py(max(o,c))`, `y2 = py(min(o,c))`, then clamp and
  give a doji one row. The first attempt (`y1 = py(g ? o : c)`) made every up-bar body collapse to
  a flat line - visible only in the zoomed preview crop, not in the full-size PNG.
* TFT_eSPI exposes no `drawPixel`; the mock TFT matches that, so single-pixel work (dithered band
  wash, dotted SMA) must go through `drawFastVLine(x, y, 1, c)`.
* Sizing a scratch buffer from a constexpr max (`chartCapMax()`) requires the source array to be
  `constexpr`: `static const uint16_t RANGE_CAP[]` is *not* a constant expression, so it failed as
  an array bound. `constexpr uint16_t RANGE_CAP[]` fixed it and costs nothing.
* `POST /api/settings` only parses the body when **Content-Type: application/json**. A plain
  `curl -d '{"backlight":80}'` (form-encoded) answers `{"ok":true}` and changes **nothing** - it
  looks like the handler is broken when the body was simply empty. Always send the header when
  probing settings over curl.
* Two cheap preview guards caught real regressions: compare the plot rectangle of a
  bands-on frame against the bands-off frame (pixels must differ), and count exact `Cfg::GREEN`
  and `Cfg::RED` pixels inside the plot - a single-colour line chart can never produce both, so
  that check proves the candlesticks were actually drawn.
* A test scenario that fakes a rising period must reverse the **whole bar**. The `S_UP` fixture
  reversed `c.v` only, leaving each bar its old open/high/low, and the line chart hid the lie;
  candlesticks turned it into a fan of giant wicks. Reverse `v`, `o`, `h`, `l` together and leave
  `t` alone, so the x axis and labels stay put.
* The README screenshots in `docs/img` are generated, not stored art: run
  `tests/preview/run_preview.py` then `tools/make_readme_img.py`, and change which frames are
  used in `SINGLES` / `GRID`. Regenerate them whenever the panel's look changes, or the README
  quietly shows an old firmware.
* Pushing this repo: the credential helper is `gh auth git-credential` (global). Running
  `GIT_TERMINAL_PROMPT=0 git push` fails with *"could not read Username for 'https://github.com'"*
  even though `gh auth status` is fine and the helper answers a manual query. Just run
  `git push origin main` plainly (it takes a few seconds).
