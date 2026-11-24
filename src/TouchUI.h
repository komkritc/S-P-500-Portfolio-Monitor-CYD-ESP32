// XPT2046 touch: calibration from claude_monitor (proven on this board),
// poll state machine producing screen / period / scroll gestures.
#pragma once
#include <Arduino.h>

namespace Touch {

enum Act { A_NONE, A_SCREEN, A_PERIOD, A_SCROLL };

void begin();
Act poll(int& arg);          // call from loop()
int pressZone();             // nav zone id currently held (-1 = none)
int pressPeriod();           // period button id currently held (-1 = none)
void setScreen(int s);       // gates vertical swipe to the positions screen

}  // namespace Touch
