// XPT2046 touch: calibration from claude_monitor (proven on this board),
// poll state machine producing screen / period / scroll gestures.
#pragma once
#include <Arduino.h>

namespace Touch {

// A_SCREEN arg is an absolute screen id (a nav-zone / dot-band tap);
// A_SWIPE arg is relative (-1 / +1). They must stay separate actions: zone id 1
// and "swipe forward" would otherwise be the same number.
enum Act { A_NONE, A_SCREEN, A_SWIPE, A_PERIOD, A_SCROLL };

void begin();
Act poll(int& arg);          // call from loop()
int pressZone();             // nav zone id currently held (-1 = none)
int pressPeriod();           // period button id currently held (-1 = none)
void setScreen(int s);       // gates vertical swipe to the positions screen

}  // namespace Touch
