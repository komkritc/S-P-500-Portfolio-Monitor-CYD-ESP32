// LCD rendering for the four screens (Dashboard / Market Chart / Positions /
// Realised P/L), drawn to match ui_esp32.png. Text goes through a slot cache so a
// 4 Hz refresh only touches pixels whose value actually changed.
#pragma once
#include <TFT_eSPI.h>
#include <Arduino.h>

extern TFT_eSPI tft;

namespace UI {

// Screen id == bottom nav zone id == page-dot id, so the nav order is the id
// order: Dashboard | Chart | Positions | Realised P/L.
// (settings.pages bitmasks are NOT the screen ids: see pagesBit() in DisplayUI.cpp.)
enum Screen {
  SC_DASH  = 0,   // nav zone 0, pages bit0
  SC_CHART = 1,   // nav zone 1, pages bit2
  SC_POS   = 2,   // nav zone 2, pages bit1
  SC_REAL  = 3,   // nav zone 3, pages bit3
  SC_COUNT = 4
};

void begin();
void bootBanner(int pct, const char* msg);  // pre-Wi-Fi boot screen
void setScreen(int s);
int screen();
void scrollPos(int dir);        // +/-1 page on the positions table
void setRange(int r);           // chart period 0=1d 1=5d 2=1mo (also tells the refresher)
int range();
void invalidate();              // drop every draw cache and repaint (after OTA/banner screens)
void update();                  // call every loop() (touch feedback + data)

}  // namespace UI
