// Layout, palette, pins — geometry comes from the CYD port brief (320x240 landscape).
#pragma once
#include <Arduino.h>
#include <stdint.h>

#include "secrets.h"  // WIFI_SSID/WIFI_PASS/TZ_OFFSET_SEC

namespace Cfg {

constexpr int16_t SCR_W = 320, SCR_H = 240;

// Global chrome
constexpr int16_t HEADER_H = 32;          // header band y 0..31
constexpr int16_t HDR_ICON_X = 10, HDR_ICON_Y = 10;  // page icon: 14 px wide (the trend dot reaches +16)
constexpr int16_t HDR_TITLE_X = 32;       // title left edge (6 px clear of the widest icon)
constexpr int16_t HDR_TIME_X = 284;       // clock right edge (wifi bars start at 287)
constexpr int16_t CONTENT_Y = 33, CONTENT_H = 172;   // y 33..204
constexpr int16_t NAV_Y = 205, NAV_H = 35;           // bottom nav band
constexpr int16_t NAV_ZONE_X[4] = {0, 80, 160, 240}; // zone id == screen id: 0 Dashboard | 1 Chart | 2 Positions | 3 Realised
constexpr int16_t NAV_ZONE_W[4] = {80, 80, 80, 80};
constexpr int16_t DOT_Y = 199;                       // page dots are not drawn any
                                                     // more; this band stays a
                                                     // touch target (see Touch::poll)

// Dashboard cards
constexpr int16_t DASH_LX = 7,  DASH_LY = 39, DASH_LW = 182, DASH_LH = 155;
constexpr int16_t DASH_RX = 195, DASH_RY = 39, DASH_RW = 118, DASH_RH = 155;
constexpr int16_t DASH_ROW_Y0 = 136, DASH_ROW_DY = 13;  // Open/High/Low/Prev rows: ink
                                                         // 139..190, card border line at
                                                         // 193 (3 px clear); divider 134

// Positions table (page 3)
constexpr int16_t TBL_X = 5, TBL_W = 310;
constexpr int16_t TBL_HY = 39, TBL_HH = 25;          // column header
constexpr int16_t TBL_ROW0 = 66, TBL_ROWH = 26, TBL_ROWS = 3;  // rows to y=144
constexpr int16_t SUM_X = 6, SUM_Y = 145, SUM_W = 308, SUM_H = 52;
// right-edge x of each column (symbol left-aligned at TBL_X, F2 <=10 chars)
constexpr int16_t COL_QTY_R = 134, COL_NAV_R = 206, COL_PL_R = 272, COL_RET_R = 315;

// Chart screen (page 2)
constexpr int16_t MK_Y = 36, MK_H = 30;              // price + change + badge

// Realised screen (page 4, mirrors the SmallTV RealisedScreen)
constexpr int16_t R_LBL_Y = 40, R_BIG_Y = 58;                         // all-time label / big number
constexpr int16_t R_C1_X = 7,   R_C1_Y = 92,  R_C1_W = 150, R_C1_H = 46;   // THIS YEAR
constexpr int16_t R_C2_X = 163, R_C2_Y = 92,  R_C2_W = 150, R_C2_H = 46;   // LIFETIME
constexpr int16_t R_C3_X = 7,   R_C3_Y = 144, R_C3_W = 150, R_C3_H = 30;   // SALES
constexpr int16_t R_C4_X = 163, R_C4_Y = 144, R_C4_W = 150, R_C4_H = 30;   // REVERSED
constexpr int16_t R_LAST_Y = 178, R_LAST_H = 26;                      // last sale strip
constexpr int16_t PBTN_Y = 69, PBTN_H = 29, PBTN_X = 7, PBTN_W = 102; // 1D/5D/1M
constexpr int16_t PLOT_X = 38, PLOT_Y = 105, PLOT_W = 279, PLOT_H = 80;
constexpr int16_t XLABEL_Y = 187;

// Palette (#rrggbb -> RGB565 at compile time)
constexpr uint16_t rgb565(uint32_t c) {
  return (uint16_t)((((c >> 16) & 0xFF) >> 3 << 11) |
                    (((c >> 8) & 0xFF) >> 2 << 5) |
                    ((c & 0xFF) >> 3));
}
constexpr uint16_t BG      = rgb565(0x05080D);
constexpr uint16_t HEADER  = rgb565(0x0A1220);
constexpr uint16_t CARD    = rgb565(0x0B1728);
constexpr uint16_t CARD2   = rgb565(0x101F33);
constexpr uint16_t BORDER  = rgb565(0x263D59);
constexpr uint16_t TEXT    = rgb565(0xF4F7FF);
constexpr uint16_t SECTXT  = rgb565(0xA9B8CC);
constexpr uint16_t CYAN    = rgb565(0x00D9FF);
constexpr uint16_t BLUE    = rgb565(0x087FFF);
constexpr uint16_t GREEN   = rgb565(0x00E887);
constexpr uint16_t RED     = rgb565(0xFF626B);
constexpr uint16_t YELLOW  = rgb565(0xFFD54A);
constexpr uint16_t MUTED   = rgb565(0x72839C);
constexpr uint16_t GREEN_BG = rgb565(0x0A2E1E);
constexpr uint16_t RED_BG   = rgb565(0x33121A);

// XPT2046 on its own VSPI bus (same wiring + calibration as claude_monitor)
constexpr uint8_t TOUCH_CLK = 25, TOUCH_MISO = 39, TOUCH_MOSI = 32,
                  TOUCH_CSPIN = 33, TOUCH_IRQ = 36;
constexpr int32_t TOUCH_X_MIN = 200, TOUCH_X_MAX = 3700;
constexpr int32_t TOUCH_Y_MIN = 240, TOUCH_Y_MAX = 3800;
constexpr uint32_t TOUCH_DEBOUNCE_MS = 200;
constexpr uint32_t TOUCH_FEEDBACK_MS = 130;
constexpr int16_t SWIPE_MIN_PX = 48;

constexpr int64_t TZ_OFFSET = TZ_OFFSET_SEC;

// mDNS / OTA identity (distinct from the ESP8266 unit's "sp500.local")
constexpr const char* HOSTNAME = "sp500-cyd";

}  // namespace Cfg
