#include "TouchUI.h"
#include "DisplayUI.h"   // UI::Screen ids - the nav order is defined there, not here
#include "config.h"
#include <XPT2046_Touchscreen.h>

namespace {

SPIClass touchSpi = SPIClass(VSPI);
XPT2046_Touchscreen ts(Cfg::TOUCH_CSPIN, Cfg::TOUCH_IRQ);

bool down = false;
int16_t sx = 0, sy = 0;        // press start (screen px)
int16_t lx = 0, ly = 0;        // last point still under the finger (screen px)
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
    lx = x; ly = y;
    zoneHeld = (y >= Cfg::NAV_Y) ? (int)((unsigned)x / 80) : -1;
    if (zoneHeld >= 4) zoneHeld = -1;
    periodHeld = (curScreen == UI::SC_CHART && inPeriodBtn(x, y))   // 1D/5D/1M live on the chart screen
                     ? periodBtnIdx(x)
                     : -1;
    if (ms - logMs > 500)
      Serial.printf("[touch] press %d,%d\n", x, y);
    logMs = ms;
    return A_NONE;
  }
  if (now && down) {            // still held: follow the finger
    lx = x; ly = y;
    return A_NONE;
  }
  if (!now && down) {
    down = false;
    const int z = zoneHeld, pr = periodHeld;
    zoneHeld = periodHeld = -1;
    if (ms - lastAccept < Cfg::TOUCH_DEBOUNCE_MS) return A_NONE;

    // readPt() leaves x,y untouched when there is no touch, so on the release
    // frame they are stale/zero - the gesture must end at the last point that
    // was actually under the finger, or every tap looks like a swipe to 0,0.
    const int dx = lx - sx, dy = ly - sy;
    if (abs(dx) >= Cfg::SWIPE_MIN_PX && abs(dx) > abs(dy)) {
      lastAccept = ms;
      arg = dx > 0 ? -1 : +1;
      Serial.printf("[touch] swipe %s (%d)\n", dx > 0 ? "right" : "left", arg);
      return A_SWIPE;
    }
    if (abs(dy) >= Cfg::SWIPE_MIN_PX && curScreen == UI::SC_POS) {
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
    // The strip the page dots used to sit on: the dots are no longer drawn, so
    // this band is split into the same four zones as the nav bar - a tap just
    // above a nav button now does what the button under it does.
    if (sy >= Cfg::DOT_Y - 6 && sy <= Cfg::DOT_Y + 6) {
      for (int i = 0; i < 4; i++)
        if (sx >= Cfg::NAV_ZONE_X[i] && sx < Cfg::NAV_ZONE_X[i] + Cfg::NAV_ZONE_W[i]) {
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
