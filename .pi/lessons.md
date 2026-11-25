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
  `python3 tools/gen_web_assets.py` (gz limit 22,528 B) before `pio run`.
* `python3 tests/run_tests.py` builds host unit tests from `src/` (879 asserts);
  `DisplayUI.cpp` is not in that file list, so the host build never compiles it.
  `python3 tests/preview/run_preview.py` renders 71 LCD frames with a mock TFT.
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
| `/data/Projects/sp500` | `.git` at that root, branch `main` | **both** project folders: `SP500_SmallTV/**` (live) and an old copy of `ESP32-CYD-SP500/**` from before the split |
| `/data/Projects/sp500/ESP32-CYD-SP500` | its own `.git` (nested) | the CYD firmware only |

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
