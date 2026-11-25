// DisplayUI.cpp - 320x240 landscape UI for the CYD (TFT_eSPI + ILI9341).
//
// All money/quote/chart numbers come from g_app (appRecompute is the only place
// arithmetic happens); this file only draws. Text is cached per slot and only
// redrawn when its content or colour changes, so the loop stays cheap.
#include "DisplayUI.h"
#include "config.h"
#include "SmoothFonts.h"
#include "TouchUI.h"
#include "core/AppState.h"
#include "core/MarketClock.h"
#include "finance/ChartData.h"
#include "finance/MarketData.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

using Cfg::BG;

namespace {

// ---- fonts: TFT_eSPI builtin ids; >= 18 = smooth Inter (SmoothFonts.h) ----
enum { F1 = 1, F2 = 2, F4 = 4, FG18 = 18, FG24 = 24 };
const uint8_t* curSmooth = nullptr;  // loaded smooth font (nullptr = none)

void useFont(uint8_t f) {
  // Anti-aliased Inter (SmoothFonts.h, gen_smooth_font.py) for the big
  // number sizes; TFT bitmap fonts for the dense small labels.
  if (f >= FG18) {
    const uint8_t* want = f == FG24 ? Font_24 : Font_18;
    if (curSmooth != want) {
      if (curSmooth) tft.unloadFont();
      tft.loadFont(want);
      curSmooth = want;
    }
    tft.setTextFont(4);  // loaded smooth font renders as "font 4"
  } else {
    if (curSmooth) {
      tft.unloadFont();
      curSmooth = nullptr;
    }
    tft.setTextFont(f);
  }
}

// Ink box of each font (height, and how far below the cursor y the ink starts),
// measured from the preview harness (DUMPBOXES=1 ./tests/preview/preview tests).
// TFT_eSPI's fontHeight() is the cell height, which is not what looks centred.
struct Ink { int h, top; };
Ink inkOf(uint8_t f) {
  switch (f) {
    case F1:   return {10, 3};
    case F2:   return {10, 3};
    case F4:   return {18, 1};
    case FG24: return {24, 5};
    default:   return {13, 5};   // FG18 (Inter 18): "1D" inks 21x13, top 5 px below y
  }
}

// ---- slot cache: draw text only when it changed -------------------------
struct Slot {
  int16_t id, x, y, w, h;
  uint16_t fg, bg;
  char txt[44];
};
Slot g_slots[72];
int g_nSlots = 0;

enum Sid {
  S_TITLE = 0, S_TIME,
  // dashboard
  S_NAV = 8, S_INV, S_CNT, S_PX, S_CHG, S_EST, S_RO, S_RH, S_RL, S_RP,
  // positions
  S_PAGE = 16, S_ROW = 20,  // S_ROW + row*5 + col
  S_TNAV = 40, S_TPL, S_TRET,
  // chart
  S_CPX = 48, S_CCHG,
  // realised
  S_RPL = 56, S_RYRL, S_RYRV, S_RLTV, S_RSALE, S_RREV,
  S_RLS1, S_RLS2, S_RLS3, S_RLS4,
};

Slot* slotById(int id) {
  for (int i = 0; i < g_nSlots; i++)
    if (g_slots[i].id == id) return &g_slots[i];
  return nullptr;
}

// x,y = top-left (or top-right when alignRight); y is a true top for both
// builtin and GFX fonts.
void txtSlot(int id, int16_t x, int16_t y, uint8_t font, uint16_t fg,
             uint16_t bg, bool alignRight, const char* s) {
  Slot* sl = slotById(id);
  if (sl && strcmp(sl->txt, s) == 0 && sl->fg == fg && sl->bg == bg) return;
  if (sl) {
    tft.fillRect(sl->x - 1, sl->y - 1, sl->w + 3, sl->h + 2, sl->bg);
  } else if (g_nSlots < 72) {
    sl = &g_slots[g_nSlots++];
    sl->id = id;
    strlcpy(sl->txt, "\x01", sizeof(sl->txt));
  } else {
    return;
  }
  useFont(font);
  const int w = tft.textWidth(s), h = tft.fontHeight();
  const int16_t x0 = alignRight ? (int16_t)(x - w) : x;
  strlcpy(sl->txt, s, sizeof(sl->txt));
  sl->x = x0; sl->y = y; sl->w = w; sl->h = h; sl->fg = fg; sl->bg = bg;
  tft.setTextColor(fg, bg);
  tft.drawString(s, x0, y);  // datum is top-left for bitmap and GFXFF alike
}

void txtFmt(int id, int16_t x, int16_t y, uint8_t font, uint16_t fg,
            uint16_t bg, bool right, const char* fmt, ...) {
  char buf[44];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  txtSlot(id, x, y, font, fg, bg, right, buf);
}

// font auto-shrink for big money numbers
uint8_t fitFont(const char* s, int maxW) {
  useFont(FG24);
  if ((int)tft.textWidth(s) <= maxW) return FG24;
  useFont(FG18);
  if ((int)tft.textWidth(s) <= maxW) return FG18;
  useFont(F4);
  if ((int)tft.textWidth(s) <= maxW) return F4;
  return F2;
}

// pick F2, else F1, for small text that must fit maxW
uint8_t fit2(const char* s, int maxW) {
  useFont(F2);
  if ((int)tft.textWidth(s) <= maxW) return F2;
  useFont(F1);
  return F1;
}

// mid-size card number: FG18, else F4, else F2
uint8_t fitCard(const char* s, int maxW) {
  useFont(FG18);
  if ((int)tft.textWidth(s) <= maxW) return FG18;
  useFont(F4);
  if ((int)tft.textWidth(s) <= maxW) return F4;
  return F2;
}

// ---- formatting ----------------------------------------------------------
void money(char* b, size_t n, double v, bool plus = false, int dec = 2) {
  if (!isNum(v)) { snprintf(b, n, "--"); return; }
  const unsigned long long p = dec ? (unsigned long long)pow(10, dec) : 1;
  const bool neg = v < 0;
  unsigned long long r = llround(fabs(v) * p);
  char digits[20];
  snprintf(digits, sizeof(digits), "%llu", r / p);
  char grouped[26];
  int gi = 0;
  for (size_t i = 0; i < strlen(digits); i++) {
    if (i && (strlen(digits) - i) % 3 == 0) grouped[gi++] = ',';
    grouped[gi++] = digits[i];
  }
  grouped[gi] = 0;
  if (dec) snprintf(b, n, "%s%s.%0*llu", neg ? "-" : (plus ? "+" : ""),
                    grouped, dec, r % p);
  else snprintf(b, n, "%s%s", neg ? "-" : (plus ? "+" : ""), grouped);
}

// abbreviated money (11.23B / 45.67M / 890K) when the grouped form cannot fit
void moneyCompact(char* b, size_t n, double v, bool plus) {
  if (!isNum(v)) { snprintf(b, n, "--"); return; }
  const double a = fabs(v);
  const char* sg = v < 0 ? "-" : (plus ? "+" : "");
  if (a >= 1e9) snprintf(b, n, "%s%.2fB", sg, a / 1e9);
  else if (a >= 1e6) snprintf(b, n, "%s%.2fM", sg, a / 1e6);
  else if (a >= 1e5) snprintf(b, n, "%s%.0fK", sg, a / 1e3);
  else money(b, n, v, plus, 2);
}

char g_numBuf[6][40];  // rotating buffers for call sites needing a string
const char* numBuf(int i, double v, bool plus = false, int dec = 2) {
  money(g_numBuf[i & 5], 40, v, plus, dec);
  return g_numBuf[i & 5];
}

// ISO "2026-11-19" -> "19/11/2026" (same format as the web dashboard).
void dateText(char* out, size_t n, const char* iso) {
  if (!iso || strlen(iso) != 10 || iso[4] != '-' || iso[7] != '-') {
    snprintf(out, n, "N/A");
    return;
  }
  snprintf(out, n, "%c%c/%c%c/%c%c%c%c", iso[8], iso[9], iso[5], iso[6],
           iso[0], iso[1], iso[2], iso[3]);
}

// ---- colour blend --------------------------------------------------------
uint16_t blend565(uint16_t a, uint16_t b, uint8_t wa) {  // wa 0..255 -> a
  const int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  const int br = (b >> 11) & 31, bg2 = (b >> 5) & 63, bb = b & 31;
  const int r = (ar * wa + br * (255 - wa)) >> 8;
  const int g = (ag * wa + bg2 * (255 - wa)) >> 8;
  const int bl = (ab * wa + bb * (255 - wa)) >> 8;
  return (uint16_t)(r << 11 | g << 5 | bl);
}

// ---- icons ---------------------------------------------------------------
void iconHome(int x, int y, uint16_t c) {  // 14x11 house
  tft.drawLine(x, y + 5, x + 7, y, c);
  tft.drawLine(x + 7, y, x + 14, y + 5, c);
  tft.drawLine(x + 2, y + 5, x + 2, y + 11, c);
  tft.drawLine(x + 12, y + 5, x + 12, y + 11, c);
  tft.drawLine(x + 2, y + 11, x + 12, y + 11, c);
  tft.drawFastVLine(x + 7, y + 7, 4, c);
}
void iconRows(int x, int y, uint16_t c) {  // table rows
  tft.fillRect(x, y + 1, 14, 2, c);
  tft.fillRect(x, y + 5, 14, 2, c);
  tft.fillRect(x, y + 9, 14, 2, c);
}
void iconTrend(int x, int y, uint16_t c) {
  tft.drawLine(x, y + 11, x + 4, y + 6, c);
  tft.drawLine(x + 4, y + 6, x + 8, y + 8, c);
  tft.drawLine(x + 8, y + 8, x + 14, y, c);
  tft.fillCircle(x + 14, y, 2, c);
}
void iconFlow(int x, int y, uint16_t c) {  // up+down arrows (realised P/L)
  tft.drawFastVLine(x + 2, y, 9, c);
  tft.drawLine(x + 2, y, x, y + 3, c);
  tft.drawLine(x + 2, y, x + 4, y + 3, c);
  tft.drawFastVLine(x + 9, y + 3, 9, c);
  tft.drawLine(x + 9, y + 12, x + 7, y + 9, c);
  tft.drawLine(x + 9, y + 12, x + 11, y + 9, c);
}
void iconBars(int x, int y, uint16_t c) {  // bar-chart (dashboard header)
  tft.fillRect(x, y + 8, 3, 4, c);
  tft.fillRect(x + 5, y + 4, 3, 8, c);
  tft.fillRect(x + 10, y, 3, 12, c);
}
void iconStack(int x, int y, uint16_t c) {  // layers (positions header)
  tft.fillRect(x, y + 9, 14, 3, c);
  tft.fillRect(x + 2, y + 5, 10, 3, c);
  tft.fillRect(x + 4, y + 1, 6, 3, c);
}
void iconScreen(int s, int x, int y, uint16_t c) {
  if (s == UI::SC_DASH) iconBars(x, y, c);
  else if (s == UI::SC_POS) iconStack(x, y, c);
  else if (s == UI::SC_REAL) iconFlow(x, y, c);
  else iconTrend(x, y, c);
}

int g_curScreen = UI::SC_DASH;
int g_posPage = 0;
int g_range = -1;  // chart period, -1 = follow settings.defaultRange
uint32_t g_lastPlot = 0;
uint32_t g_rotMs = 0;  // last screen switch (user or rotation)
int g_wifiBars = -1, g_hdrIconScr = -1;

// settings.pages / settings.charts are the web-UI bitmasks (docs/API.md):
//   pages:  bit0 Dashboard, bit1 Positions, bit2 Chart, bit3 Realised  (0 = all)
//   charts: bit0 1D, bit1 5D, bit2 1M                                   (0 = all)
// They steer only the automatic rotation; touching a nav zone or a period
// button always works, so a mask can never lock the user out of a screen.
// The bit of a page is fixed by the web UI, NOT by the LCD nav order
// (on the LCD the order is Dashboard, Chart, Positions, Realised).
int pagesBit(int s) {
  switch (s) {
    case UI::SC_DASH:  return 0;
    case UI::SC_POS:   return 1;
    case UI::SC_CHART: return 2;
    default:           return 3;   // UI::SC_REAL
  }
}

int pagesMask()  { const uint8_t p = g_app.settings.pages;       return p ? p : 15; }
int chartsMask() { const uint8_t c = g_app.settings.charts & 7u; return c ? c : 7; }

int firstEnabledRange(int from) {
  const int cm = chartsMask();
  for (int k = 0; k < RANGE_COUNT; k++) { const int r = (from + k) % RANGE_COUNT; if (cm & (1 << r)) return r; }
  return from;
}

// rotation calls this when it ENTERS the chart screen: step to the next
// enabled range, so the rotation cycles 1D -> 5D -> 1M like the SmallTV does
void advanceRange() {
  const int cm = chartsMask();
  if (g_range < 0) g_range = firstEnabledRange(g_app.settings.defaultRange);
  else {
    for (int k = 1; k <= RANGE_COUNT; k++) {
      const int r = (g_range + k) % RANGE_COUNT;
      if (cm & (1 << r)) { g_range = r; break; }
    }
  }
  market::setViewRange((uint8_t)g_range);  // the visible chart gets refresh priority
  g_lastPlot = 0;
}

int curRange() {
  if (g_range < 0) g_range = firstEnabledRange(g_app.settings.defaultRange);
  return g_range;
}

void drawWifi() {
  const NetInfo& n = g_app.net;
  int bars = 1;
  uint16_t on = Cfg::RED;  // offline / still connecting
  if (n.connected) {
    on = Cfg::CYAN;
    bars = n.rssi > -55 ? 4 : n.rssi > -67 ? 3 : n.rssi > -80 ? 2 : 1;
  } else if (n.mode == NetInfo::NET_AP_SETUP ||
             n.mode == NetInfo::NET_AP_RESCUE) {
    on = Cfg::YELLOW;
    bars = 4;
  }
  const int state = bars | (on << 8);
  if (state == g_wifiBars) return;
  g_wifiBars = state;
  tft.fillRect(286, 8, 22, 16, Cfg::HEADER);
  const uint16_t dim = Cfg::MUTED;
  for (int i = 0; i < 4; i++) {
    const int h = 4 + i * 3;
    tft.fillRect(287 + i * 5, 22 - h, 4, h, i < bars ? on : dim);
  }
}

// ---- chrome --------------------------------------------------------------
void drawHeader() {
  tft.fillRect(0, 0, Cfg::SCR_W, Cfg::HEADER_H, Cfg::HEADER);
  tft.drawFastHLine(0, Cfg::HEADER_H, Cfg::SCR_W, Cfg::BORDER);
  iconScreen(g_curScreen, Cfg::HDR_ICON_X, Cfg::HDR_ICON_Y, Cfg::CYAN);
  g_hdrIconScr = g_curScreen;
  drawWifi();
}

void drawNav() {
  tft.fillRoundRect(0, Cfg::NAV_Y, Cfg::SCR_W, Cfg::NAV_H, 10, Cfg::HEADER);
  tft.fillRect(0, Cfg::NAV_Y, Cfg::SCR_W, 10, Cfg::HEADER);  // square top edge
  tft.drawFastHLine(0, Cfg::NAV_Y - 1, Cfg::SCR_W, Cfg::BORDER);
  static const char* names[4] = {"Dashboard", "Chart", "Positions", "Realised"};
  const int held = Touch::pressZone();
  useFont(F2);
  for (int i = 0; i < 4; i++) {
    const int zx = Cfg::NAV_ZONE_X[i], zw = Cfg::NAV_ZONE_W[i];
    const bool act = g_curScreen == i;
    if (act) {
      const uint16_t fill = held == i ? blend565(Cfg::TEXT, Cfg::BLUE, 70)
                                      : Cfg::BLUE;
      tft.fillRoundRect(zx + 4, Cfg::NAV_Y + 3, zw - 8, Cfg::NAV_H - 7, 8,
                        fill);
      tft.drawRoundRect(zx + 4, Cfg::NAV_Y + 3, zw - 8, Cfg::NAV_H - 7, 8,
                        Cfg::CYAN);
    }
    const uint16_t col = (act || held == i) ? Cfg::TEXT : Cfg::SECTXT;
    const uint16_t ic = (act || held == i) ? Cfg::TEXT : Cfg::MUTED;
    if (i == 0) iconHome(zx + zw / 2 - 7, Cfg::NAV_Y + 7, ic);
    else if (i == 1) iconTrend(zx + zw / 2 - 7, Cfg::NAV_Y + 7, ic);
    else if (i == 2) iconRows(zx + zw / 2 - 7, Cfg::NAV_Y + 7, ic);
    else iconFlow(zx + zw / 2 - 7, Cfg::NAV_Y + 7, ic);
    const int tw = (int)tft.textWidth(names[i]);
    tft.setTextColor(col, Cfg::HEADER);
    tft.drawString(names[i], zx + (zw - tw) / 2, Cfg::NAV_Y + 22, 2);
  }
}

// The page-indicator dots were dropped from the UI; the strip at DOT_Y is still
// a touch target (see Touch::poll), so tapping there still switches pages.

// ---- status badge ---------------------------------------------------------
uint16_t badgeColor(MarketStatus s, uint16_t* bg) {
  switch (s) {
    case MS_OPEN: *bg = Cfg::GREEN_BG; return Cfg::GREEN;
    case MS_CLOSED: *bg = Cfg::RED_BG; return Cfg::RED;
    case MS_PRE: *bg = Cfg::CARD2; return Cfg::YELLOW;
    default: *bg = Cfg::CARD2; return Cfg::MUTED;
  }
}
const char* badgeName(MarketStatus s) {
  switch (s) {
    case MS_OPEN: return "OPEN";
    case MS_CLOSED: return "CLOSED";
    case MS_PRE: return "PRE-MKT";
    default: return "N/A";
  }
}

MarketStatus g_badgeLast = (MarketStatus)-1;
int g_badgeX = -1, g_badgeY = -1, g_badgeW = 0;
uint16_t g_badgeBg = Cfg::CARD;

void drawBadge(int xRight, int y, MarketStatus s, uint16_t cardBg, bool hidePre = false) {
  uint16_t cbg;
  const uint16_t c = badgeColor(s, &cbg);
  if (hidePre && s == MS_PRE) {
    // dashboard choice: pre-market covers most of the day, a permanent chip is clutter.
    // Clear any chip left by a previous OPEN/CLOSED state so the card never keeps a stale badge.
    if (g_badgeW > 0 && g_badgeY == y)
      tft.fillRect(g_badgeX - g_badgeW, y - 3, g_badgeW, 22, g_badgeBg);
    g_badgeLast = s; g_badgeX = xRight; g_badgeY = y; g_badgeW = 0; g_badgeBg = cardBg;
    return;
  }
  if (s == g_badgeLast && xRight == g_badgeX && y == g_badgeY) return;
  if (g_badgeW > 0 && g_badgeY == y)
    tft.fillRect(g_badgeX - g_badgeW, y - 3, g_badgeW, 22, g_badgeBg);
  useFont(F2);
  const char* name = badgeName(s);
  const int w = (int)tft.textWidth(name) + 14, h = 16;
  tft.fillRoundRect(xRight - w, y, w, h, 8, cbg);
  tft.setTextColor(c, cbg);
  tft.drawString(name, xRight - w + 7, y + (h - 10) / 2, 2);
  g_badgeLast = s; g_badgeX = xRight; g_badgeY = y; g_badgeW = w;
  g_badgeBg = cardBg;
}

// ---- dashboard -----------------------------------------------------------
void drawDashStatics() {
  tft.fillRoundRect(Cfg::DASH_LX, Cfg::DASH_LY, Cfg::DASH_LW, Cfg::DASH_LH,
                    10, Cfg::CARD);
  tft.drawRoundRect(Cfg::DASH_LX, Cfg::DASH_LY, Cfg::DASH_LW, Cfg::DASH_LH,
                    10, Cfg::BORDER);
  tft.fillRoundRect(Cfg::DASH_RX, Cfg::DASH_RY, Cfg::DASH_RW, Cfg::DASH_RH,
                    10, Cfg::CARD);
  tft.drawRoundRect(Cfg::DASH_RX, Cfg::DASH_RY, Cfg::DASH_RW, Cfg::DASH_RH,
                    10, Cfg::BORDER);
  useFont(F2);
  tft.setTextColor(Cfg::CYAN, Cfg::CARD);
  tft.drawString("PORTFOLIO NAV", Cfg::DASH_LX + 12, 46, 2);
  tft.drawString("INVESTED", Cfg::DASH_LX + 12, 150, 2);
  tft.drawString("POSITIONS", Cfg::DASH_LX + Cfg::DASH_LW - 12 -
                                             (int)tft.textWidth("POSITIONS"),
                 150, 2);
  tft.setTextColor(Cfg::CYAN, Cfg::CARD);
  tft.drawString("S&P 500", Cfg::DASH_RX + 10, 45, 2);
  tft.drawFastHLine(Cfg::DASH_RX + 10, 134, Cfg::DASH_RW - 20, Cfg::BORDER);
  const char* rows[4] = {"Open", "High", "Low", "Prev"};
  for (int i = 0; i < 4; i++) {
    tft.setTextColor(Cfg::SECTXT, Cfg::CARD);
    tft.drawString(rows[i], Cfg::DASH_RX + 10, Cfg::DASH_ROW_Y0 + i * Cfg::DASH_ROW_DY, 2);
  }
}

char lastChipKey[64];

void drawDashDynamic() {
  const PortfolioSummary& sm = g_app.summary;
  const QuoteView& qv = g_app.quote;

  // NAV big number (shrink font to fit the card)
  char navS[40];
  money(navS, sizeof(navS), sm.nav);
  txtSlot(S_NAV, Cfg::DASH_LX + 12, 66, fitFont(navS, 158), Cfg::TEXT,
          Cfg::CARD, false, navS);

  // P/L chip: full repaint only when its text changes
  char key[64];
  snprintf(key, sizeof(key), "%s|%s", numBuf(1, sm.pl, true),
           numBuf(2, sm.retPct, true, 2));
  if (strcmp(key, lastChipKey) != 0) {
    strlcpy(lastChipKey, key, sizeof(lastChipKey));
    tft.fillRect(Cfg::DASH_LX + 10, 102, 162, 48, Cfg::CARD);
    const bool ok = isNum(sm.pl);
    const bool up = ok && sm.pl >= 0;
    const uint16_t c = ok ? (up ? Cfg::GREEN : Cfg::RED) : Cfg::MUTED;
    const uint16_t cbg = ok ? (up ? Cfg::GREEN_BG : Cfg::RED_BG) : Cfg::CARD2;
    tft.fillRoundRect(Cfg::DASH_LX + 12, 104, 158, 44, 8, cbg);
    useFont(F2);
    tft.setTextColor(Cfg::SECTXT, cbg);
    tft.drawString("Unrealised P/L", Cfg::DASH_LX + 22, 110, 2);
    if (ok) {
      char pc[24];
      // the % sits right-aligned on the same line as the label: drop decimals
      // before it would run into the label ink (+12.36% -> +12.4% -> +12%)
      const int labR = Cfg::DASH_LX + 22 + (int)tft.textWidth("Unrealised P/L");
      const int budget = Cfg::DASH_LX + 158 - labR - 6;
      snprintf(pc, sizeof(pc), "%+.2f%%", sm.retPct);
      if ((int)tft.textWidth(pc) > budget) snprintf(pc, sizeof(pc), "%+.1f%%", sm.retPct);
      if ((int)tft.textWidth(pc) > budget) snprintf(pc, sizeof(pc), "%+.0f%%", sm.retPct);
      tft.setTextColor(c, cbg);
      tft.drawString(pc, Cfg::DASH_LX + 158 - (int)tft.textWidth(pc), 110, 2);
      useFont(FG18);
      tft.setTextColor(c, cbg);
      const char* pls = numBuf(3, sm.pl, true);
      uint8_t pf = FG18;
      if ((int)tft.textWidth(pls) > 132) {
        useFont(F4);
        pf = F4;
        if ((int)tft.textWidth(pls) > 132) pf = F2;
      }
      useFont(pf);
      tft.drawString(pls, Cfg::DASH_LX + 22, pf >= FG18 ? 124 : 128);  // smooth y = cell top
    } else {
      tft.setTextColor(Cfg::MUTED, cbg);
      tft.drawString("waiting for quotes", Cfg::DASH_LX + 22, 130, 2);
    }
  }

  // INVESTED / POSITIONS values: their y sits 18 px under the label cursor,
  // because F2 "10,600,000" style numbers get a comma that descends into the
  // value's row. Invested is shown in whole baht - the decimals only push the
  // number into the POSITIONS column next to it.
  txtFmt(S_INV, Cfg::DASH_LX + 12, 168, F2, Cfg::TEXT, Cfg::CARD, false,
         "%s", numBuf(4, sm.invested, false, 0));
  txtFmt(S_CNT, Cfg::DASH_LX + Cfg::DASH_LW - 12, 168, F2, Cfg::TEXT, Cfg::CARD,
         true, "%u", (unsigned)sm.count);

  // S&P card. The ^GSPC line was dropped, so the price block (price, change,
  // quote time) is spread through the card instead of hugging the divider:
  // label ink ends 58, price 67-83 (Inter 18: the card is only 118 px wide, so
  // Inter 24 cannot fit a 5-digit index), change 95-105, time 114-126,
  // divider 136, then the four OHLC rows at DASH_ROW_Y0/DY (ink 141..190,
  // 4 px clear of the card border at 194).
  if (qv.valid) {
    char pxS[40];
    money(pxS, sizeof(pxS), qv.last);
    txtSlot(S_PX, Cfg::DASH_RX + 10, 62, fitFont(pxS, Cfg::DASH_RW - 20),
            Cfg::TEXT, Cfg::CARD, false, pxS);
    const uint16_t cc = qv.change >= 0 ? Cfg::GREEN : Cfg::RED;
    char chs[60];
    if (isNum(qv.change))
      snprintf(chs, sizeof(chs), "%s (%+.2f%%)", numBuf(1, qv.change, true),
               qv.changePct);
    else
      snprintf(chs, sizeof(chs), "%s", numBuf(1, qv.last));
    uint8_t cf = fit2(chs, Cfg::DASH_RW - 20);
    if (cf == F1 && (int)tft.textWidth(chs) > Cfg::DASH_RW - 20 &&
        isNum(qv.change)) {
      char cc2[24];
      moneyCompact(cc2, sizeof(cc2), qv.change, true);
      snprintf(chs, sizeof(chs), "%s (%+.1f%%)", cc2, qv.changePct);
      cf = F1;
    }
    txtSlot(S_CHG, Cfg::DASH_RX + 10, 92, cf, cc, Cfg::CARD, false, chs);
    char est[40];
    if (qv.marketTime)
      formatQuoteTime(est, sizeof(est), qv.marketTime, g_app.settings.tzMode,
                      g_app.settings.localOffsetMin, false);
    else
      est[0] = 0;
    txtSlot(S_EST, Cfg::DASH_RX + 10, 113, F2, Cfg::MUTED, Cfg::CARD, false,
            est);
    char nb[20];
    const struct { double v; int id; } rv[4] = {
        {qv.open, S_RO}, {qv.high, S_RH}, {qv.low, S_RL}, {qv.prevClose, S_RP}};
    for (int i = 0; i < 4; i++) {
      if (!isNum(rv[i].v))
        txtSlot(rv[i].id, Cfg::DASH_RX + Cfg::DASH_RW - 10, Cfg::DASH_ROW_Y0 + i * Cfg::DASH_ROW_DY, F2,
                Cfg::SECTXT, Cfg::CARD, true, "--");
      else {
        money(nb, sizeof(nb), rv[i].v);
        uint8_t f = fit2(nb, 56);
        if (f == F1 && (int)tft.textWidth(nb) > 56) {
          moneyCompact(nb, sizeof(nb), rv[i].v, false);
          f = F1;
        }
        txtSlot(rv[i].id, Cfg::DASH_RX + Cfg::DASH_RW - 10, Cfg::DASH_ROW_Y0 + i * Cfg::DASH_ROW_DY, f,
                Cfg::TEXT, Cfg::CARD, true, nb);
      }
    }
  } else {
    txtSlot(S_PX, Cfg::DASH_RX + 10, 62, FG18, Cfg::MUTED, Cfg::CARD, false,
            "--");
    txtSlot(S_CHG, Cfg::DASH_RX + 10, 92, F2, Cfg::MUTED, Cfg::CARD, false,
            "fetching...");
    txtSlot(S_EST, Cfg::DASH_RX + 10, 113, F2, Cfg::CARD, Cfg::CARD, false, "");
    for (int i = 0; i < 4; i++)
      txtSlot(S_RO + i, Cfg::DASH_RX + Cfg::DASH_RW - 10, Cfg::DASH_ROW_Y0 + i * Cfg::DASH_ROW_DY, F2,
              Cfg::CARD, Cfg::CARD, true, "");
  }
  drawBadge(Cfg::DASH_LX + Cfg::DASH_LW - 10, 45, qv.status, Cfg::CARD, true);
}

// ---- positions -----------------------------------------------------------
void drawPosStatics() {
  useFont(F2);
  tft.setTextColor(Cfg::SECTXT, Cfg::BG);
  tft.drawString("SYMBOL", Cfg::TBL_X + 8, Cfg::TBL_HY + 8, 2);
  const char* lbl[3] = {"QTY", "NAV", "P/L"};   // the currency lives in the summary card, not every column
  const int rx[3] = {Cfg::COL_QTY_R, Cfg::COL_NAV_R, Cfg::COL_PL_R};
  for (int i = 0; i < 3; i++)
    tft.drawString(lbl[i], rx[i] - (int)tft.textWidth(lbl[i]),
                   Cfg::TBL_HY + 8, 2);
  tft.drawString("RET", Cfg::COL_RET_R - (int)tft.textWidth("RET"),
                 Cfg::TBL_HY + 8, 2);
  tft.drawFastHLine(Cfg::TBL_X, Cfg::TBL_HY + Cfg::TBL_HH - 1, Cfg::TBL_W,
                    Cfg::BORDER);
  for (int r = 0; r < Cfg::TBL_ROWS; r++)
    tft.drawFastHLine(Cfg::TBL_X, Cfg::TBL_ROW0 + r * Cfg::TBL_ROWH + 25,
                      Cfg::TBL_W, Cfg::BORDER);
  tft.fillRoundRect(Cfg::SUM_X, Cfg::SUM_Y, Cfg::SUM_W, Cfg::SUM_H, 8,
                    Cfg::CARD);
  tft.drawRoundRect(Cfg::SUM_X, Cfg::SUM_Y, Cfg::SUM_W, Cfg::SUM_H, 8,
                    Cfg::BORDER);
  tft.setTextColor(Cfg::SECTXT, Cfg::CARD);
  const char* sl[3] = {"TOTAL NAV (THB)", "UNREALISED P/L (THB)", "RETURN %"};
  for (int i = 0; i < 3; i++)
    tft.drawString(sl[i], Cfg::SUM_X + 8, Cfg::SUM_Y + 5 + i * 15, 2);
}

void drawPosDynamic() {
  const int n = g_app.posCount;
  const int pages = n ? (n + Cfg::TBL_ROWS - 1) / Cfg::TBL_ROWS : 1;
  if (g_posPage >= pages) g_posPage = pages - 1;
  if (g_posPage < 0) g_posPage = 0;

  if (n == 0) {
    txtFmt(S_PAGE, 95, 110, F2, Cfg::MUTED, Cfg::BG, false,
           "no positions - add via web");
    for (int r = 0; r < Cfg::TBL_ROWS; r++) {
      const int y = Cfg::TBL_ROW0 + r * Cfg::TBL_ROWH + 5;
      for (int c = 0; c < 5; c++)
        txtSlot(S_ROW + r * 5 + c, 8, y, F2, Cfg::BG, Cfg::BG, false, "");
    }
  } else {
    txtFmt(S_PAGE, Cfg::COL_QTY_R, 33, F2, Cfg::MUTED, Cfg::BG, true,
           "p %d/%d", g_posPage + 1, pages);
    for (int r = 0; r < Cfg::TBL_ROWS; r++) {
      const int idx = g_posPage * Cfg::TBL_ROWS + r;
      const int y = Cfg::TBL_ROW0 + r * Cfg::TBL_ROWH + 5;
      if (idx >= n) {
        for (int c = 0; c < 5; c++)
          txtSlot(S_ROW + r * 5 + c, 8, y, F2, Cfg::BG, Cfg::BG, false, "");
        continue;
      }
      const Position& p = g_app.positions[idx];
      const PositionResult& rr = g_app.results[idx];
      const bool hasQ = isNum(rr.price);
      const uint16_t pc = rr.pl >= 0 ? Cfg::GREEN : Cfg::RED;
      txtFmt(S_ROW + r * 5 + 0, Cfg::TBL_X + 8, y, F2, Cfg::TEXT, Cfg::BG,
             false, "%.10s", p.symbol);
      char cb[40];
      snprintf(cb, sizeof(cb), p.qty < 1000 ? "%.2f" : "%.0f", p.qty);
      {
        uint8_t f = fit2(cb, 54);
        if (f == F1 && (int)tft.textWidth(cb) > 54) {
          moneyCompact(cb, 40, p.qty, false);
          f = F1;
        }
        txtSlot(S_ROW + r * 5 + 1, Cfg::COL_QTY_R, y + 2, f,
                hasQ ? Cfg::TEXT : Cfg::SECTXT, Cfg::BG, true, cb);
      }
      snprintf(cb, sizeof(cb), "%s", numBuf(0, rr.nav, false, 0));
      {
        uint8_t f = fit2(cb, 60);
        if (f == F1 && (int)tft.textWidth(cb) > 60) {
          moneyCompact(cb, 40, rr.nav, false);
          f = F1;
        }
        txtSlot(S_ROW + r * 5 + 2, Cfg::COL_NAV_R, y + 2, f,
                hasQ ? Cfg::TEXT : Cfg::MUTED, Cfg::BG, true, cb);
      }
      snprintf(cb, sizeof(cb), "%s", numBuf(1, rr.pl, true, 0));
      {
        uint8_t f = fit2(cb, 56);
        if (f == F1 && (int)tft.textWidth(cb) > 56) {
          moneyCompact(cb, 40, rr.pl, true);
          f = F1;
        }
        txtSlot(S_ROW + r * 5 + 3, Cfg::COL_PL_R, y + 2, f,
                hasQ ? pc : Cfg::MUTED, Cfg::BG, true, cb);
      }
      txtFmt(S_ROW + r * 5 + 4, Cfg::COL_RET_R, y + 2, F2,
             hasQ ? pc : Cfg::MUTED, Cfg::BG, true,
             fabs(rr.retPct) >= 100 ? "%+.0f%%" : "%+.1f%%", rr.retPct);
    }
  }
  const PortfolioSummary& sm = g_app.summary;
  txtFmt(S_TNAV, Cfg::SUM_X + Cfg::SUM_W - 8, Cfg::SUM_Y + 5, F2, Cfg::TEXT,
         Cfg::CARD, true, "%s", numBuf(2, sm.nav));
  txtFmt(S_TPL, Cfg::SUM_X + Cfg::SUM_W - 8, Cfg::SUM_Y + 20, F2,
         sm.pl >= 0 ? Cfg::GREEN : Cfg::RED, Cfg::CARD, true, "%s",
         numBuf(3, sm.pl, true));
  txtFmt(S_TRET, Cfg::SUM_X + Cfg::SUM_W - 8, Cfg::SUM_Y + 35, F2,
         sm.retPct >= 0 ? Cfg::GREEN : Cfg::RED, Cfg::CARD, true, "%+.2f%%",
         sm.retPct);
}

// ---- realised (page 4: mirrors the SmallTV RealisedScreen) ----------------
void drawRealStatics() {
  useFont(F2);
  tft.setTextColor(Cfg::SECTXT, Cfg::BG);
  tft.drawString("ALL-TIME REALISED P/L", 10, Cfg::R_LBL_Y, 2);
  const struct { int x, y, w, h; const char* lbl; } cards[4] = {
      {Cfg::R_C1_X, Cfg::R_C1_Y, Cfg::R_C1_W, Cfg::R_C1_H, "THIS YEAR"},
      {Cfg::R_C2_X, Cfg::R_C2_Y, Cfg::R_C2_W, Cfg::R_C2_H, "LIFETIME"},
      {Cfg::R_C3_X, Cfg::R_C3_Y, Cfg::R_C3_W, Cfg::R_C3_H, "SALES"},
      {Cfg::R_C4_X, Cfg::R_C4_Y, Cfg::R_C4_W, Cfg::R_C4_H, "REVERSED"},
  };
  for (const auto& c : cards) {
    tft.fillRoundRect(c.x, c.y, c.w, c.h, 8, Cfg::CARD);
    tft.drawRoundRect(c.x, c.y, c.w, c.h, 8, Cfg::BORDER);
    tft.setTextColor(Cfg::SECTXT, Cfg::CARD);
    tft.drawString(c.lbl, c.x + 9, c.y + 5, 2);
  }
  tft.drawFastHLine(0, Cfg::R_LAST_Y - 3, Cfg::SCR_W, Cfg::BORDER);
}

static double yearPl(const RealisedSummary& s, int year) {
  if (year <= 0) return NAN;
  for (uint8_t i = 0; i < s.nYears; i++)
    if (s.year[i] == (uint16_t)year) return s.yearPl[i];
  return 0;  // known: no sale fell into this year
}

void drawRealDynamic() {
  const RealisedSummary& s = g_app.realised;
  char b[40];

  const int cy = (g_app.clockSynced && g_app.nowEpoch)
                     ? toLocal(g_app.nowEpoch,
                               displayOffsetSec(g_app.nowEpoch,
                                                g_app.settings.tzMode,
                                                g_app.settings.localOffsetMin))
                           .y
                     : 0;
  txtFmt(S_RYRL, Cfg::R_C1_X + Cfg::R_C1_W - 9, Cfg::R_C1_Y + 5, F2, Cfg::MUTED,
         Cfg::CARD, true, cy ? "%d" : "N/A", cy);

  if (!s.valid || s.count == 0) {
    // no readable ledger: never a made-up 0
    txtSlot(S_RPL, 10, Cfg::R_BIG_Y, FG18, Cfg::MUTED, Cfg::BG, false,
            s.valid ? "no sales yet" : "ledger unreadable");
    txtSlot(S_RYRV, Cfg::R_C1_X + 9, Cfg::R_C1_Y + 25, FG18, Cfg::MUTED,
            Cfg::CARD, false, "--");
    txtSlot(S_RLTV, Cfg::R_C2_X + 9, Cfg::R_C2_Y + 25, FG18, Cfg::MUTED,
            Cfg::CARD, false, "--");
    txtSlot(S_RSALE, Cfg::R_C3_X + Cfg::R_C3_W - 9, Cfg::R_C3_Y + 4, FG18,
            Cfg::TEXT, Cfg::CARD, true, "0");
    txtSlot(S_RREV, Cfg::R_C4_X + Cfg::R_C4_W - 9, Cfg::R_C4_Y + 4, FG18,
            Cfg::TEXT, Cfg::CARD, true, "0");
    txtSlot(S_RLS1, 10, Cfg::R_LAST_Y + 3, F1, Cfg::MUTED, Cfg::BG, false,
            "no sales recorded");
    txtSlot(S_RLS2, 310, Cfg::R_LAST_Y + 3, F1, Cfg::BG, Cfg::BG, true, "");
    txtSlot(S_RLS3, 10, Cfg::R_LAST_Y + 18, F1, Cfg::BG, Cfg::BG, false, "");
    txtSlot(S_RLS4, 310, Cfg::R_LAST_Y + 18, F1, Cfg::BG, Cfg::BG, true, "");
    return;
  }

  money(b, sizeof(b), s.pl, true);
  txtSlot(S_RPL, 10, Cfg::R_BIG_Y, fitFont(b, 300),
          s.pl >= 0 ? Cfg::GREEN : Cfg::RED, Cfg::BG, false, b);

  const double ypl = yearPl(s, cy);
  {
    const char* vs = numBuf(0, ypl, true);
    txtSlot(S_RYRV, Cfg::R_C1_X + 9, Cfg::R_C1_Y + 22,
            fitCard(vs, Cfg::R_C1_W - 18),
            isNum(ypl) ? (ypl >= 0 ? Cfg::GREEN : Cfg::RED) : Cfg::MUTED,
            Cfg::CARD, false, vs);
    // lifetime = unrealised + realised (computed once in appRecompute)
    const double lt = g_app.summary.lifetimePl;
    const char* ls = numBuf(1, lt, true);
    txtSlot(S_RLTV, Cfg::R_C2_X + 9, Cfg::R_C2_Y + 22,
            fitCard(ls, Cfg::R_C2_W - 18),
            isNum(lt) ? (lt >= 0 ? Cfg::GREEN : Cfg::RED) : Cfg::MUTED,
            Cfg::CARD, false, ls);
  }

  txtFmt(S_RSALE, Cfg::R_C3_X + Cfg::R_C3_W - 9, Cfg::R_C3_Y + 4, FG18,
         Cfg::TEXT, Cfg::CARD, true, "%u", (unsigned)s.count);
  txtFmt(S_RREV, Cfg::R_C4_X + Cfg::R_C4_W - 9, Cfg::R_C4_Y + 4, FG18,
         s.reversed ? Cfg::YELLOW : Cfg::TEXT, Cfg::CARD, true, "%u",
         (unsigned)s.reversed);

  if (s.hasLast) {
    char lab[40];
    snprintf(lab, sizeof(lab), "LAST SALE %s", s.lastId);
    txtSlot(S_RLS1, 10, Cfg::R_LAST_Y + 3, F1, Cfg::MUTED, Cfg::BG, false, lab);
    dateText(b, sizeof(b), s.lastDate);
    txtSlot(S_RLS2, 310, Cfg::R_LAST_Y + 3, F1, Cfg::MUTED, Cfg::BG, true, b);
    txtFmt(S_RLS3, 10, Cfg::R_LAST_Y + 18, F1, Cfg::TEXT, Cfg::BG, false,
           "%.12s", s.lastSymbol);
    txtFmt(S_RLS4, 310, Cfg::R_LAST_Y + 18, F1,
           s.lastRealised >= 0 ? Cfg::GREEN : Cfg::RED, Cfg::BG, true, "%s",
           numBuf(2, s.lastRealised, true));
  }
}

// ---- chart ---------------------------------------------------------------
char g_pbtnNames[3][8] = {"1D", "5D", "1M"};

// Bollinger Band scratch for the LCD plot (sized for the largest stored range: 5D = 112 bars,
// ~1.3 KB DRAM for all three). Written by bollinger() on every plot repaint while the setting
// is on; the leading p-1 entries stay NaN and are never drawn.
float g_bbMid[chartCapMax()], g_bbUp[chartCapMax()], g_bbDn[chartCapMax()];

// the 1D / 5D / 1M labels: Inter 18 (a size down from the old font 4), font 4
// then font 2 if a label is ever too wide for its card
uint8_t fitBtn(const char* s, int maxW) {
  useFont(FG18);
  if ((int)tft.textWidth(s) <= maxW) return FG18;
  useFont(F4);
  if ((int)tft.textWidth(s) <= maxW) return F4;
  useFont(F2);
  return F2;
}

void drawPeriodButtons() {
  const int act = curRange();
  const int held = Touch::pressPeriod();
  for (int i = 0; i < 3; i++) {
    const int x = Cfg::PBTN_X + i * (Cfg::PBTN_W + 1);
    const bool a = act == i;
    uint16_t fill = a ? Cfg::BLUE : Cfg::CARD;
    if (held == i) fill = blend565(Cfg::TEXT, a ? Cfg::BLUE : Cfg::CARD, 70);
    tft.fillRoundRect(x, Cfg::PBTN_Y, Cfg::PBTN_W, Cfg::PBTN_H, 8, fill);
    tft.drawRoundRect(x, Cfg::PBTN_Y, Cfg::PBTN_W, Cfg::PBTN_H, 8,
                      a ? Cfg::CYAN : Cfg::BORDER);
    const uint8_t f = fitBtn(g_pbtnNames[i], Cfg::PBTN_W - 14);
    useFont(f);
    const Ink K = inkOf(f);
    const int tw = (int)tft.textWidth(g_pbtnNames[i]);
    const int ty = Cfg::PBTN_Y + (Cfg::PBTN_H - K.h) / 2 - K.top;
    tft.setTextColor(a || held == i ? Cfg::TEXT : Cfg::SECTXT, fill);
    tft.drawString(g_pbtnNames[i], x + (Cfg::PBTN_W - tw) / 2, ty, f);
  }
}

void drawPlot() {
  const int r = curRange();
  const Chart& c = g_app.charts[r];
  // clear plot + label area
  tft.fillRect(Cfg::PLOT_X - 1, Cfg::PLOT_Y - 2, Cfg::PLOT_W + 2,
               Cfg::PLOT_H + 14, Cfg::BG);
  if (!c.valid || c.n < 2) {
    useFont(F2);
    tft.setTextColor(Cfg::SECTXT, Cfg::BG);
    tft.drawString("waiting for data...", 100, 140, 2);
    return;
  }
  const int n = c.n;

  // Bollinger Bands on the panel (web Settings -> "BB + candles on the LCD chart"). The period is the
  // setting, clamped to n/2: 1M stores only 24 daily bars, so a 50-period band would mostly
  // cover its own window - on the panel a band is only honest if the window is well under half
  // the series. Below 6 bars nothing is drawn at all. The corner tag states the period actually
  // used, so the screen never claims a band it did not draw.
  uint8_t bbP = 0;
  if (g_app.settings.bbLcd && n >= 6) {
    const int p = (int)g_app.settings.bbLcdP < n / 2 ? (int)g_app.settings.bbLcdP : n / 2;
    if (p >= 2) bbP = (uint8_t)p;
  }
  const uint16_t bbMade = bbP ? bollinger(c.v, n, bbP, g_bbMid, g_bbUp, g_bbDn) : 0;
  const int bbFrom = n - (int)bbMade;          // first index with a complete band

  // Candlesticks on the panel: with Bollinger Bands on, the Market page is being read as a
  // technical chart, so the same OHLC the web dashboard draws becomes candles here too - one
  // switch, one visual language, and the bands sit on bars that already show open/high/low.
  // A series whose bars carry no wick (h == l, e.g. a close-only aggregation) keeps the line +
  // fill, because candles without bodies would be nothing but dots.
  bool candles = false;
  if (bbMade && c.o && c.h && c.l) {
    int wick = 0;
    for (int i = 0; i < n; i++)
      if (c.h[i] > c.l[i] && c.h[i] >= c.o[i] && c.l[i] <= c.o[i] && ++wick >= 4) { candles = true; break; }
  }

  float mn = 1e30f, mx = -1e30f;
  for (int i = 0; i < n; i++) {
    if (c.v[i] < mn) mn = c.v[i];
    if (c.v[i] > mx) mx = c.v[i];
    if (candles) {                              // the wicks have to fit inside the plot too
      if (c.h[i] > mx) mx = c.h[i];
      if (c.l[i] < mn) mn = c.l[i];
    }
  }
  if (bbMade)                                    // the bands must fit inside the plot too
    for (int i = bbFrom; i < n; i++) {
      if (g_bbUp[i] > mx) mx = g_bbUp[i];
      if (g_bbDn[i] < mn) mn = g_bbDn[i];
    }
  const float pad = (mx - mn) * 0.06f + 0.01f;
  mn -= pad; mx += pad;
  const auto py = [&](float v) {
    return (int16_t)(Cfg::PLOT_Y + Cfg::PLOT_H -
                     (v - mn) / (mx - mn) * (Cfg::PLOT_H - 1));
  };

  // horizontal grid + y labels (font1, no commas)
  useFont(F1);
  const int dec = (mx - mn) >= 100 ? 0 : ((mx - mn) >= 1 ? 2 : 4);
  for (int i = 0; i < 5; i++) {
    const int y = Cfg::PLOT_Y + Cfg::PLOT_H - i * Cfg::PLOT_H / 4;
    tft.drawFastHLine(Cfg::PLOT_X, y, Cfg::PLOT_W, Cfg::HEADER);
    char lb[12];
    money(lb, sizeof(lb), mn + (mx - mn) * i / 4, false, dec);
    const int w = (int)tft.textWidth(lb);
    tft.setTextColor(Cfg::MUTED, Cfg::BG);
    tft.drawString(lb, Cfg::PLOT_X - 3 - w, y - 4 - (i == 0 ? 2 : 0), 1);
  }

  // x mapping per period + labels
  auto tX = [&](int i) -> int16_t {
    if (r == RANGE_1D) {
      const int off = nyOffsetSec(c.t[i]);
      const int m = (int)(((c.t[i] + off) % 86400) / 60);
      float f = (m - 570) / 390.0f;
      if (f < 0) f = 0;
      if (f > 1) f = 1;
      return (int16_t)(Cfg::PLOT_X + 1 + f * (Cfg::PLOT_W - 2));
    }
    return (int16_t)(Cfg::PLOT_X + 1 +
                     (float)i / (n - 1) * (Cfg::PLOT_W - 2));
  };

  char lab[3][16];
  int labX[3];
  if (r == RANGE_1D) {
    for (int k = 0; k < 3; k++) {
      const int m = 570 + k * 195;  // 09:30 12:45 16:00
      snprintf(lab[k], 16, "%02d:%02d", m / 60, m % 60);
      labX[k] = (int)(Cfg::PLOT_X + 1 +
                      (m - 570) / 390.0f * (Cfg::PLOT_W - 2));
    }
  } else if (r == RANGE_5D) {
    // label the first bar of each trading day (up to 3 spread across)
    int bIdx[6], bN = 0;
    long day = -1;
    for (int i = 0; i < n && bN < 6; i++) {
      const long d = (c.t[i] + nyOffsetSec(c.t[i])) / 86400;
      if (d != day) { day = d; bIdx[bN++] = i; }
    }
    for (int k = 0; k < 3; k++) {
      const int i = bN > 1 ? bIdx[k * (bN - 1) / 2] : 0;
      formatMonthDay(lab[k], 16, c.t[i], g_app.settings.tzMode,
                     g_app.settings.localOffsetMin);
      labX[k] = tX(i);
    }
  } else {
    for (int k = 0; k < 3; k++) {
      const int i = k * (n - 1) / 2;
      formatMonthDay(lab[k], 16, c.t[i], g_app.settings.tzMode,
                     g_app.settings.localOffsetMin);
      labX[k] = tX(i);
    }
  }
  useFont(F1);
  for (int k = 0; k < 3; k++) {
    tft.drawFastVLine(labX[k], Cfg::PLOT_Y, Cfg::PLOT_H, Cfg::HEADER);
    tft.setTextColor(Cfg::MUTED, Cfg::BG);
    int lx = labX[k] - (int)tft.textWidth(lab[k]) / 2;
    lx = constrain(lx, 0, Cfg::SCR_W - 40);
    tft.drawString(lab[k], lx, Cfg::XLABEL_Y, 1);
  }

  // Line + fill colour follows the sign of the period change, the same number
  // the header shows: 1D = last vs the previous close, 5D / 1M = last vs the
  // first bar (Chart.base is only set for 1D; computeChartStats falls back to
  // the first point). Green when flat or up, red when down.
  const ChartStats& st = g_app.chartStats[r];
  const bool up = isNum(st.change) ? (st.change >= 0) : (c.v[n - 1] >= c.v[0]);
  const uint16_t col = up ? Cfg::GREEN : Cfg::RED;
  const uint16_t dim = blend565(col, Cfg::BG, 46);
  if (candles) {
    // One bar per stored point: a wick from low to high, a filled body open -> close,
    // coloured per bar (green up, red down). Bodies are widened by the average pitch but
    // always leave a gap, so 112 bars in 279 px still read as a row of separate bars.
    int bw = 1;
    if (n > 1) {
      const int pitch = (abs((int)tX(n - 1) - (int)tX(0))) / (n - 1);
      bw = pitch > 1 ? pitch - 1 : 1;
      if (bw > 9) bw = 9;                       // 1M (24 bars) would otherwise be fat blocks
    }
    const int yTop = Cfg::PLOT_Y, yBot = Cfg::PLOT_Y + Cfg::PLOT_H;
    for (int i = 0; i < n; i++) {
      float o = c.o[i], hh = c.h[i], ll = c.l[i];
      if (!isNum(o)) o = c.v[i];                // a bar with no open degenerates to a line
      if (!isNum(hh) || hh < ll) { hh = c.v[i]; ll = c.v[i]; }
      const bool g = c.v[i] >= o;
      const uint16_t cc = g ? Cfg::GREEN : Cfg::RED;
      int yh = py(hh), yl = py(ll);
      if (yh > yl) { const int t = yh; yh = yl; yl = t; }      // py inverts: high -> small y
      // body: the higher of open/close is the top pixel row, the lower is the bottom.
      // A doji (open == close) still gets one row, so the body never disappears.
      const float bTop = o > c.v[i] ? o : c.v[i], bBot = o > c.v[i] ? c.v[i] : o;
      int y1 = py(bTop), y2 = py(bBot);
      if (y2 < y1) { const int t = y2; y2 = y1; y1 = t; }
      y1 = constrain(y1, yTop, yBot); y2 = constrain(y2, yTop, yBot);
      if (y2 - y1 < 1 && y2 < yBot) y2 = y1 + 1;
      yh = constrain(yh, yTop, yBot); yl = constrain(yl, yTop, yBot);
      const int x = tX(i), x0 = bw > 1 ? x - bw / 2 : x;
      tft.drawFastVLine(x, yh, yl - yh + 1, cc);
      for (int k = 0; k < bw; k++) tft.drawFastVLine(x0 + k, y1, y2 - y1 + 1, cc);
    }
  } else {
  int16_t px0 = -1, py0 = 0;
  for (int i = 0; i < n; i++) {
    const int16_t X = tX(i), Y = py(c.v[i]);
    if (px0 >= 0) {
      tft.drawLine(px0, py0, X, Y, col);
      // vertical fill under this span (dense: 1px columns)
      const int a = px0 < X ? px0 : X, b = px0 > X ? px0 : X;
      for (int cx = a; cx <= b; cx++) {
        const float f = b == a ? 0 : (float)(cx - a) / (b - a);
        const int yl = py0 + (int)((Y - py0) * f);
        if (Cfg::PLOT_Y + Cfg::PLOT_H - yl > 1)
          tft.drawFastVLine(cx, yl + 1, Cfg::PLOT_Y + Cfg::PLOT_H - yl - 1, dim);
      }
    }
    px0 = X; py0 = Y;
  }
  tft.fillCircle(px0, py0, 2, col);
  }

  // ---- Bollinger Bands, drawn after the price line and its fill (same layer order as the
  // web chart) so they stay visible. The panel has no alpha blending, so the band interior is
  // painted as a dithered wash: every other pixel of a checkerboard, phase fixed by x+y, which
  // reads as a soft band without hiding the fill under the price line.
  if (bbMade) {
    const uint16_t wash = blend565(Cfg::CYAN, Cfg::BG, 60);   // faintest: interior only
    const uint16_t edge = blend565(Cfg::CYAN, Cfg::BG, 190);  // upper / lower line
    const uint16_t midc = blend565(Cfg::CYAN, Cfg::BG, 120);  // the SMA, dotted
    const int16_t yTop = Cfg::PLOT_Y, yBot = Cfg::PLOT_Y + Cfg::PLOT_H;
    for (int i = bbFrom; i < n; i++) {
      const int16_t x = tX(i);
      int ya = py(g_bbUp[i]), yb = py(g_bbDn[i]);
      if (ya < yTop) ya = yTop;
      if (yb > yBot) yb = yBot;
      for (int y = ya; y <= yb; y++)
        if (((x + y) & 1) == 0) tft.drawFastVLine(x, y, 1, wash);
    }
    for (int i = bbFrom + 1; i < n; i++) {
      tft.drawLine(tX(i - 1), py(g_bbUp[i - 1]), tX(i), py(g_bbUp[i]), edge);
      tft.drawLine(tX(i - 1), py(g_bbDn[i - 1]), tX(i), py(g_bbDn[i]), edge);
    }
    for (int i = bbFrom; i < n; i++) {           // no dash primitive: punch dots along the SMA
      const int x0 = tX(i - 1 < bbFrom ? bbFrom : i - 1), y0 = py(g_bbMid[i - 1 < bbFrom ? bbFrom : i - 1]);
      const int x1 = tX(i), y1 = py(g_bbMid[i]);
      const int steps = abs(x1 - x0) + abs(y1 - y0);
      for (int s = 3; s <= steps; s += 3) {
        const int x = x0 + (x1 - x0) * s / steps, y = y0 + (y1 - y0) * s / steps;
        if (y >= yTop && y <= yBot) tft.drawFastVLine(x, y, 1, midc);
      }
    }
    char tg[10];
    snprintf(tg, sizeof(tg), "BB %d", (int)bbP);   // sigma is fixed on the panel (Cfg plot is only 80 px tall)
    useFont(F1);
    tft.setTextColor(Cfg::MUTED, Cfg::BG);
    const int tw = (int)tft.textWidth(tg);
    tft.drawString(tg, Cfg::PLOT_X + Cfg::PLOT_W - tw - 1, Cfg::PLOT_Y + 1, 1);
  }
}

uint32_t plotSig() {
  const Chart& c = g_app.charts[curRange()];
  return (uint32_t)curRange() * 7u + c.n * 131u + c.okEpoch;
}

// page rotation skips screens that have nothing to show (matches the
// SmallTV ViewSchedule rule: the realised page only exists once a sale stands)
// and screens the "LCD pages to show" checkboxes turned off (settings.pages)
bool viewEnabled(int s) {
  if (!(pagesMask() & (1 << pagesBit(s)))) return false;
  if (s == UI::SC_POS) return g_app.posCount > 0;
  if (s == UI::SC_REAL) return g_app.realised.valid && g_app.realised.count > 0;
  return true;
}

// header title on every page: Inter 18 - a size down from font 4, so it cannot
// crowd the page icon on its left or the clock on its right; font 2 is the
// last-resort shrink for a long title (e.g. "Positions (12)").
void drawHeaderTitle(const char* s) {
  useFont(FG18);
  uint8_t f = FG18;
  const Slot* t = slotById(S_TIME);              // the clock is drawn just before this
  const int budget = (t ? (int)t->x : 206) - 6 - Cfg::HDR_TITLE_X;
  if ((int)tft.textWidth(s) > budget) { useFont(F2); f = F2; }
  const Ink K = inkOf(f);
  const int ty = (Cfg::HEADER_H - K.h) / 2 - K.top;
  txtSlot(S_TITLE, Cfg::HDR_TITLE_X, ty, f, Cfg::TEXT, Cfg::HEADER, false, s);
}

}  // namespace

namespace UI {

void begin() {
  setBacklight(g_app.settings.backlight);   // honour the saved dim level (after tft.init)
  tft.fillScreen(BG);
  drawDashStatics();
  drawHeader();
  drawNav();
  if (g_range < 0) g_range = firstEnabledRange(g_app.settings.defaultRange);
}

void bootBanner(int pct, const char* msg) {
  tft.fillScreen(BG);
  useFont(F4);
  tft.setTextColor(Cfg::CYAN, BG);
  const char* t = "S&P 500 Portfolio";
  tft.drawString(t, (Cfg::SCR_W - (int)tft.textWidth(t)) / 2, 92, 4);
  useFont(F2);
  tft.setTextColor(Cfg::SECTXT, BG);
  tft.drawString(msg, (Cfg::SCR_W - (int)tft.textWidth(msg)) / 2, 116, 2);
  tft.drawRoundRect(60, 140, 200, 12, 6, Cfg::BORDER);
  if (pct > 0)
    tft.fillRoundRect(62, 142, (196 * pct) / 100, 8, 4, Cfg::BLUE);
}

// ---- LCD backlight (dim) -------------------------------------------------
// TFT_eSPI only ever switches the backlight on (a digitalWrite in initDisplay), so
// dimming is ours: LEDC PWM on the CYD's TFT_BL (GPIO 21), 5 kHz / 10 bit.
// An LED string's light output is roughly linear in duty, while the eye follows
// roughly output^0.35 - so a linear slider would read near-full until its last few
// ticks. Squaring the position spreads the perceived steps evenly: 50 % is a quarter
// of the light, 75 % a little over half. The mapping runs from the floor duty at 5 %
// up to full at 100 %, so the bottom of the slider always does something and the
// panel can never go fully dark (the web UI is the only way back). 24/1024 = 2.3 %
// duty stays well above LED dropout at 5 kHz. ESP32 core 2.x API; the core 3.x
// names sit behind a version guard, and the host preview harness (no ESP32, no
// TFT_BL) still runs the maths with the writes guarded out.
enum { BL_PWM_CH = 0, BL_PWM_HZ = 5000, BL_PWM_RES = 10,
       BL_MIN_DUTY = 24, BL_MAX_DUTY = 1023 };
int g_blPct = -1;

void setBacklight(int pct) {
  if (pct < (int)BL_MIN_PCT) pct = (int)BL_MIN_PCT;
  if (pct > (int)BL_MAX_PCT) pct = (int)BL_MAX_PCT;
  if (pct == g_blPct) return;                     // LEDC untouched unless it changed
  g_blPct = pct;
  const float t = (float)(pct - (int)BL_MIN_PCT) / (float)((int)BL_MAX_PCT - (int)BL_MIN_PCT);
  const unsigned d = (unsigned)lroundf((float)BL_MIN_DUTY + (BL_MAX_DUTY - BL_MIN_DUTY) * t * t);
  Serial.printf("[ui] backlight %d%% duty=%u\n", pct, d);
#if defined(ESP32) && defined(TFT_BL)
  static bool attached = false;
  if (!attached) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(TFT_BL, BL_PWM_HZ, BL_PWM_RES);
#else
    ledcSetup(BL_PWM_CH, BL_PWM_HZ, BL_PWM_RES);
    ledcAttachPin(TFT_BL, BL_PWM_CH);
#endif
    attached = true;   // must happen after tft.init(): that claims TFT_BL as OUTPUT first
  }
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(TFT_BL, d);
#else
  ledcWrite(BL_PWM_CH, d);
#endif
#else
  (void)d;   // host preview harness: the curve runs, there is just no PWM to drive
#endif
}

int backlightPct() { return g_blPct; }

void setScreen(int s) {
  if (s < 0) s = 0;
  if (s >= SC_COUNT) s = SC_COUNT - 1;
  if (s == g_curScreen) return;
  g_curScreen = s;
  g_posPage = 0;
  g_lastPlot = 0;
  g_rotMs = millis();
  Touch::setScreen(s);
  g_nSlots = 0;
  lastChipKey[0] = 0;
  g_badgeLast = (MarketStatus)-1;  // the whole screen is wiped and repainted below
  // Full-screen wipe, not just the content band: the smooth TTF fonts leave anti-aliased
  // pixels outside their nominal ink boxes, so a page's ink can bleed into the header and
  // nav band. Clearing only the content band used to leave faint chart pixels on the
  // Positions page after a rotation (user report, fw 1.3.4). One full fill at a page
  // switch costs ~20 ms on the SPI bus - invisible, and it kills the whole bug class.
  tft.fillScreen(BG);
  if (s == SC_DASH) drawDashStatics();
  else if (s == SC_POS) drawPosStatics();
  else if (s == SC_REAL) drawRealStatics();
  else drawPeriodButtons();
  drawHeader();
  drawNav();
}

int screen() { return g_curScreen; }

void invalidate() {
  g_nSlots = 0;
  lastChipKey[0] = 0;
  g_badgeLast = (MarketStatus)-1;
  g_lastPlot = 0;
  tft.fillScreen(BG);
  if (g_curScreen == SC_DASH) drawDashStatics();
  else if (g_curScreen == SC_POS) drawPosStatics();
  else if (g_curScreen == SC_REAL) drawRealStatics();
  else drawPeriodButtons();
  drawHeader();
  drawNav();
}

// Repaint the Market chart only - used when a setting that changes the plot itself arrives from
// the web UI (Bollinger Bands). No full-screen flash: the next UI::update() redraws the plot.
void invalidateChart() { g_lastPlot = 0; }

void scrollPos(int dir) {
  g_posPage += dir;
  if (g_posPage < 0) g_posPage = 0;
}

void setRange(int r) {
  if (r < 0 || r >= RANGE_COUNT || r == curRange()) return;
  g_range = r;
  market::setViewRange((uint8_t)r);  // the visible chart gets refresh priority
  g_lastPlot = 0;
  g_rotMs = millis();
  if (g_curScreen == SC_CHART) drawPeriodButtons();
}

int range() { return curRange(); }

void update() {
  static int lastZone = -2, lastPr = -2, lastScr = -2;
  static uint32_t lastData = 0;

  // ---- touch: dispatch actions, then repaint pressed states ----
  int arg = 0;
  switch (Touch::poll(arg)) {
    case Touch::A_SWIPE:
      setScreen(g_curScreen + arg);            // relative: -1 / +1
      break;
    case Touch::A_SCREEN:
      setScreen(arg);                          // absolute nav zone / dot band id
      break;
    case Touch::A_SCROLL:
      scrollPos(arg);
      break;
    case Touch::A_PERIOD:
      setRange(arg);
      break;
    default:
      break;
  }

  const int z = Touch::pressZone();
  const int pr = Touch::pressPeriod();
  const int scr = g_curScreen;
  if (z != lastZone || scr != lastScr) {
    lastZone = z; lastScr = scr;
    drawNav();
  }
  if (scr == SC_CHART && pr != lastPr) {
    lastPr = pr;
    drawPeriodButtons();
  }

  // ---- automatic page rotation (0 = off) ----
  const uint32_t ms = millis();
  const uint16_t rot = g_app.settings.pageRotateS;
  if (rot && ms - g_rotMs >= (uint32_t)rot * 1000UL) {
    g_rotMs = ms;
    for (int k = 1; k < SC_COUNT; k++) {
      const int t = (g_curScreen + k) % SC_COUNT;
      if (viewEnabled(t)) { if (t == SC_CHART) advanceRange(); setScreen(t); break; }
    }
  }

  if (ms - lastData < 400) return;
  lastData = ms;

  // header slots (clock follows the display-timezone setting)
  char lt[24];
  if (g_app.clockSynced && g_app.nowEpoch) {
    const int32_t off = displayOffsetSec(g_app.nowEpoch,
                                         g_app.settings.tzMode,
                                         g_app.settings.localOffsetMin);
    const LocalTime L = toLocal(g_app.nowEpoch, off);
    static const char* const MN[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                                       "Jul","Aug","Sep","Oct","Nov","Dec"};
    snprintf(lt, sizeof(lt), "%s %d, %02d:%02d", MN[L.m - 1], L.d, L.hh, L.mm);
  } else {
    snprintf(lt, sizeof(lt), "--:--");
  }
  txtSlot(S_TIME, Cfg::HDR_TIME_X, 8, F2, Cfg::TEXT, Cfg::HEADER, true, lt);

  switch (scr) {
    case SC_DASH:
      drawHeaderTitle("S&P 500");
      drawDashDynamic();
      break;
    case SC_POS: {
      char t[24];
      snprintf(t, sizeof(t), "Positions (%u)", (unsigned)g_app.posCount);
      drawHeaderTitle(t);
      drawPosDynamic();
      break;
    }
    case SC_REAL:
      drawHeaderTitle("Realised P/L");
      drawRealDynamic();
      break;
    default: {
      drawHeaderTitle("Market Chart");
      const QuoteView& qv = g_app.quote;
      if (qv.valid) {
        txtFmt(S_CPX, 7, Cfg::MK_Y, FG18, Cfg::TEXT, Cfg::BG, false, "%s",
               numBuf(0, qv.last));
        useFont(FG18);
        const int pw = (int)tft.textWidth(numBuf(0, qv.last));
        if (isNum(qv.change)) {
          const int cx = 7 + pw + 12;
          const int budget = Cfg::SCR_W - 60 - cx;  // leave room for the badge
          char cs[70];
          snprintf(cs, sizeof(cs), "%s (%+.2f%%)", numBuf(1, qv.change, true),
                   qv.changePct);
          uint8_t f = fit2(cs, budget);
          if (f == F1 && (int)tft.textWidth(cs) > budget) {
            char cc2[24];
            moneyCompact(cc2, sizeof(cc2), qv.change, true);
            snprintf(cs, sizeof(cs), "%s %+.1f%%", cc2, qv.changePct);
            f = F1;
          }
          txtSlot(S_CCHG, cx, Cfg::MK_Y + 8, f,
                  qv.change >= 0 ? Cfg::GREEN : Cfg::RED, Cfg::BG, false, cs);
        } else
          txtSlot(S_CCHG, 7 + pw + 12, Cfg::MK_Y + 8, F2, Cfg::MUTED, Cfg::BG,
                  false, "");
      } else {
        txtSlot(S_CPX, 7, Cfg::MK_Y, FG18, Cfg::MUTED, Cfg::BG, false, "--");
        txtSlot(S_CCHG, 7 + 60, Cfg::MK_Y + 8, F2, Cfg::MUTED, Cfg::BG, false,
                "waiting for data");
      }
      drawBadge(Cfg::SCR_W - 5, Cfg::MK_Y + 4, qv.status, Cfg::BG);
      const uint32_t sig = plotSig();
      if (sig != g_lastPlot) {
        g_lastPlot = sig;
        drawPlot();
      }
      break;
    }
  }
}

}  // namespace UI
