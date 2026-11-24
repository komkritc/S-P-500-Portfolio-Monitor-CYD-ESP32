#include "TouchUI.h"
#include "config.h"
#include <XPT2046_Touchscreen.h>

namespace {

SPIClass touchSpi = SPIClass(VSPI);
XPT2046_Touchscreen ts(Cfg::TOUCH_CSPIN, Cfg::TOUCH_IRQ);

bool down = false;
int16_t sx = 0, sy = 0;        // press start (screen px)
uint32_t downMs = 0, lastAccept = 0;
int zoneHeld = -1, periodHeld = -1;
int curScreen = 0;
uint32_t logMs = 0;

bool readPt(int16_t& x, int16_t& y) {
  if (!ts.touched()) return false;
  const TS_Point p = ts.getPoint();
  long mx = map(p.x, Cfg::TOUCH_X_MIN, Cfg::TOUCH_X_MAX, 0, Cfg::SCR_W - 1);
  long my = map(p.y, Cfg::TOUCH_Y_MIN, Cfg::TOUCH_Y_MAX, 0, Cfg::SCR_H - 1);
  x = (int16_t)constrain(mx, 0, Cfg::SCR_W - 1);
  y = (int16_t)constrain(my, 0, Cfg::SCR_H - 1);
  return true;
}

bool inPeriodBtn(int x, int y) {
  return y >= Cfg::PBTN_Y && y < Cfg::PBTN_Y + Cfg::PBTN_H &&
         x >= Cfg::PBTN_X && x < Cfg::PBTN_X + 3 * (Cfg::PBTN_W + 1);
}
int periodBtnIdx(int x) { return (x - Cfg::PBTN_X) / (Cfg::PBTN_W + 1); }

}  // namespace

namespace Touch {

void begin() {
  pinMode(Cfg::TOUCH_IRQ, INPUT);
  touchSpi.begin(Cfg::TOUCH_CLK, Cfg::TOUCH_MISO, Cfg::TOUCH_MOSI,
                 Cfg::TOUCH_CSPIN);
  const bool ok = ts.begin(touchSpi);
  ts.setRotation(1);  // matches tft.setRotation(1)
  Serial.printf("[touch] XPT2046 begin=%d irq=%d\n", (int)ok,
                (int)digitalRead(Cfg::TOUCH_IRQ));
}

void setScreen(int s) { curScreen = s; }

Act poll(int& arg) {
  int16_t x = 0, y = 0;
  const bool now = readPt(x, y);
  const uint32_t ms = millis();

  if (now && !down) {
    down = true;
    downMs = ms;
    sx = x; sy = y;
    zoneHeld = (y >= Cfg::NAV_Y) ? (int)((unsigned)x / 80) : -1;
    if (zoneHeld >= 4) zoneHeld = -1;
    periodHeld = (curScreen == 3 && inPeriodBtn(x, y))   // 3 = chart screen
                     ? (x < 109 ? 0 : x < 210 ? 1 : 2)
                     : -1;
    if (ms - logMs > 500)
      Serial.printf("[touch] press %d,%d\n", x, y);
    logMs = ms;
    return A_NONE;
  }
  if (!now && down) {
    down = false;
    const int z = zoneHeld, pr = periodHeld;
    zoneHeld = periodHeld = -1;
    if (ms - lastAccept < Cfg::TOUCH_DEBOUNCE_MS) return A_NONE;

    const int dx = x - sx, dy = y - sy;
    if (abs(dx) >= Cfg::SWIPE_MIN_PX && abs(dx) > abs(dy)) {
      lastAccept = ms;
      arg = dx > 0 ? -1 : +1;
      Serial.printf("[touch] swipe %s (%d)\n", dx > 0 ? "right" : "left", arg);
      return A_SCREEN;
    }
    if (abs(dy) >= Cfg::SWIPE_MIN_PX && curScreen == 1) {
      lastAccept = ms;
      arg = dy > 0 ? -1 : +1;   // finger up -> next page
      return A_SCROLL;
    }
    // tap: hold zones were computed at press time (stable target)
    if (z >= 0) {
      lastAccept = ms;
      arg = z;
      return A_SCREEN;
    }
    if (pr >= 0) {
      lastAccept = ms;
      arg = pr;
      return A_PERIOD;
    }
    // dashboard dots
    if (y >= Cfg::DOT_Y - 6 && y <= Cfg::DOT_Y + 6) {
      for (int i = 0; i < 4; i++)
        if (abs(sx - Cfg::DOT_X[i]) <= 8) {
          lastAccept = ms;
          arg = i;
          return A_SCREEN;
        }
    }
    Serial.printf("[touch] tap %d,%d no target\n", sx, sy);
    return A_NONE;
  }
  return A_NONE;
}

int pressZone() { return zoneHeld; }
int pressPeriod() { return periodHeld; }

}  // namespace Touch
