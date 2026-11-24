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
  return F4;
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

int curRange() {
  if (g_range < 0) g_range = g_app.settings.defaultRange;
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
  iconScreen(g_curScreen, 10, 10, Cfg::CYAN);
  g_hdrIconScr = g_curScreen;
  drawWifi();
}

void drawNav() {
  tft.fillRoundRect(0, Cfg::NAV_Y, Cfg::SCR_W, Cfg::NAV_H, 10, Cfg::HEADER);
  tft.fillRect(0, Cfg::NAV_Y, Cfg::SCR_W, 10, Cfg::HEADER);  // square top edge
  tft.drawFastHLine(0, Cfg::NAV_Y - 1, Cfg::SCR_W, Cfg::BORDER);
  static const char* names[4] = {"Dashboard", "Positions", "Realised", "Chart"};
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
    else if (i == 1) iconRows(zx + zw / 2 - 7, Cfg::NAV_Y + 7, ic);
    else if (i == 2) iconFlow(zx + zw / 2 - 7, Cfg::NAV_Y + 7, ic);
    else iconTrend(zx + zw / 2 - 7, Cfg::NAV_Y + 7, ic);
    const int tw = (int)tft.textWidth(names[i]);
    tft.setTextColor(col, Cfg::HEADER);
    tft.drawString(names[i], zx + (zw - tw) / 2, Cfg::NAV_Y + 22, 2);
  }
}

void drawDots() {
  for (int i = 0; i < 4; i++) {
    if (i == g_curScreen) tft.fillCircle(Cfg::DOT_X[i], Cfg::DOT_Y, 3, Cfg::CYAN);
    else tft.drawCircle(Cfg::DOT_X[i], Cfg::DOT_Y, 3, Cfg::BORDER);
  }
}

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

void drawBadge(int xRight, int y, MarketStatus s, uint16_t cardBg) {
  uint16_t cbg;
  const uint16_t c = badgeColor(s, &cbg);
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
  useFont(F4);
  tft.setTextColor(Cfg::CYAN, Cfg::CARD);
  tft.drawString("PORTFOLIO NAV", Cfg::DASH_LX + 12, 47, 4);
  useFont(F2);
  tft.setTextColor(Cfg::SECTXT, Cfg::CARD);
  tft.drawString("INVESTED (THB)", Cfg::DASH_LX + 12, 150, 2);
  tft.drawString("POSITIONS", Cfg::DASH_LX + 104, 150, 2);
  tft.setTextColor(Cfg::CYAN, Cfg::CARD);
  tft.drawString("S&P 500", Cfg::DASH_RX + 10, 45, 4);
  tft.setTextColor(Cfg::SECTXT, Cfg::CARD);
  tft.drawString("(^GSPC)", Cfg::DASH_RX + 10, 64, 2);
  tft.drawFastHLine(Cfg::DASH_RX + 10, 136, Cfg::DASH_RW - 20, Cfg::BORDER);
  const char* rows[4] = {"Open", "High", "Low", "Prev Close"};
  for (int i = 0; i < 4; i++) {
    tft.setTextColor(Cfg::SECTXT, Cfg::CARD);
    tft.drawString(rows[i], Cfg::DASH_RX + 10, 140 + i * 14, 2);
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
      snprintf(pc, sizeof(pc), "%+.2f%%", sm.retPct);
      tft.setTextColor(c, cbg);
      tft.drawString(pc, Cfg::DASH_LX + 158 - (int)tft.textWidth(pc), 110, 2);
      useFont(FG18);
      tft.setTextColor(c, cbg);
      tft.drawString(numBuf(3, sm.pl, true), Cfg::DASH_LX + 22, 140);  // GFX baseline
    } else {
      tft.setTextColor(Cfg::MUTED, cbg);
      tft.drawString("waiting for quotes", Cfg::DASH_LX + 22, 130, 2);
    }
  }

  txtFmt(S_INV, Cfg::DASH_LX + 12, 162, F2, Cfg::TEXT, Cfg::CARD, false,
         "%s", numBuf(4, sm.invested));
  txtFmt(S_CNT, Cfg::DASH_LX + 104, 162, F2, Cfg::TEXT, Cfg::CARD, false,
         "%u", (unsigned)sm.count);

  // S&P card
  if (qv.valid) {
    txtFmt(S_PX, Cfg::DASH_RX + 10, 82, FG18, Cfg::TEXT, Cfg::CARD, false,
           "%s", numBuf(0, qv.last));
    const uint16_t cc = qv.change >= 0 ? Cfg::GREEN : Cfg::RED;
    if (isNum(qv.change))
      txtFmt(S_CHG, Cfg::DASH_RX + 10, 112, F2, cc, Cfg::CARD, false,
             "%s (%+.2f%%)", numBuf(1, qv.change, true), qv.changePct);
    else
      txtFmt(S_CHG, Cfg::DASH_RX + 10, 112, F2, Cfg::MUTED, Cfg::CARD, false,
             "%s", numBuf(1, qv.last));
    char est[40];
    if (qv.marketTime)
      formatQuoteTime(est, sizeof(est), qv.marketTime, g_app.settings.tzMode,
                      g_app.settings.localOffsetMin);
    else
      est[0] = 0;
    txtSlot(S_EST, Cfg::DASH_RX + 10, 124, F2, Cfg::MUTED, Cfg::CARD, false,
            est);
    char nb[20];
    const struct { double v; int id; } rv[4] = {
        {qv.open, S_RO}, {qv.high, S_RH}, {qv.low, S_RL}, {qv.prevClose, S_RP}};
    for (int i = 0; i < 4; i++) {
      if (!isNum(rv[i].v))
        txtSlot(rv[i].id, Cfg::DASH_RX + Cfg::DASH_RW - 10, 140 + i * 14, F2,
                Cfg::SECTXT, Cfg::CARD, true, "--");
      else {
        money(nb, sizeof(nb), rv[i].v);
        txtSlot(rv[i].id, Cfg::DASH_RX + Cfg::DASH_RW - 10, 140 + i * 14, F2,
                Cfg::TEXT, Cfg::CARD, true, nb);
      }
    }
  } else {
    txtSlot(S_PX, Cfg::DASH_RX + 10, 82, FG18, Cfg::MUTED, Cfg::CARD, false,
            "--");
    txtSlot(S_CHG, Cfg::DASH_RX + 10, 112, F2, Cfg::MUTED, Cfg::CARD, false,
            "fetching...");
    txtSlot(S_EST, Cfg::DASH_RX + 10, 124, F2, Cfg::CARD, Cfg::CARD, false, "");
    for (int i = 0; i < 4; i++)
      txtSlot(S_RO + i, Cfg::DASH_RX + Cfg::DASH_RW - 10, 140 + i * 14, F2,
              Cfg::CARD, Cfg::CARD, true, "");
  }
  drawBadge(Cfg::DASH_RX + Cfg::DASH_RW - 10, 45, qv.status, Cfg::CARD);
}

// ---- positions -----------------------------------------------------------
void drawPosStatics() {
  useFont(F2);
  tft.setTextColor(Cfg::SECTXT, Cfg::BG);
  tft.drawString("SYMBOL", Cfg::TBL_X + 8, Cfg::TBL_HY + 8, 2);
  const char* lbl[3] = {"QTY", "NAV (THB)", "P/L (THB)"};
  const int rx[3] = {Cfg::COL_QTY_R, Cfg::COL_NAV_R, Cfg::COL_PL_R};
  for (int i = 0; i < 3; i++)
    tft.drawString(lbl[i], rx[i] - (int)tft.textWidth(lbl[i]),
                   Cfg::TBL_HY + 8, 2);
  tft.drawString("RETURN", Cfg::COL_RET_R - (int)tft.textWidth("RETURN"),
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
      txtFmt(S_ROW + r * 5 + 0, Cfg::TBL_X + 8, y, F4, Cfg::TEXT, Cfg::BG,
             false, "%.12s", p.symbol);
      txtFmt(S_ROW + r * 5 + 1, Cfg::COL_QTY_R, y + 2, F2,
             hasQ ? Cfg::TEXT : Cfg::SECTXT, Cfg::BG, true, "%.3f", p.qty);
      txtFmt(S_ROW + r * 5 + 2, Cfg::COL_NAV_R, y + 2, F2,
             hasQ ? Cfg::TEXT : Cfg::MUTED, Cfg::BG, true, "%s",
             numBuf(0, rr.nav));
      txtFmt(S_ROW + r * 5 + 3, Cfg::COL_PL_R, y + 2, F2,
             hasQ ? pc : Cfg::MUTED, Cfg::BG, true, "%s",
             numBuf(1, rr.pl, true));
      txtFmt(S_ROW + r * 5 + 4, Cfg::COL_RET_R, y + 2, F2,
             hasQ ? pc : Cfg::MUTED, Cfg::BG, true, "%+.2f%%", rr.retPct);
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
    txtSlot(S_RYRV, Cfg::R_C1_X + 9, Cfg::R_C1_Y + 22, F4, Cfg::MUTED,
            Cfg::CARD, false, "--");
    txtSlot(S_RLTV, Cfg::R_C2_X + 9, Cfg::R_C2_Y + 22, F4, Cfg::MUTED,
            Cfg::CARD, false, "--");
    txtSlot(S_RSALE, Cfg::R_C3_X + 9, Cfg::R_C3_Y + 15, F4,
            Cfg::TEXT, Cfg::CARD, false, "0");
    txtSlot(S_RREV, Cfg::R_C4_X + 9, Cfg::R_C4_Y + 15, F4,
            Cfg::TEXT, Cfg::CARD, false, "0");
    txtSlot(S_RLS1, 10, Cfg::R_LAST_Y + 1, F2, Cfg::MUTED, Cfg::BG, false,
            "no sales recorded");
    txtSlot(S_RLS2, 310, Cfg::R_LAST_Y + 1, F2, Cfg::BG, Cfg::BG, true, "");
    txtSlot(S_RLS3, 10, Cfg::R_LAST_Y + 15, F2, Cfg::BG, Cfg::BG, false, "");
    txtSlot(S_RLS4, 310, Cfg::R_LAST_Y + 15, F2, Cfg::BG, Cfg::BG, true, "");
    return;
  }

  money(b, sizeof(b), s.pl, true);
  txtSlot(S_RPL, 10, Cfg::R_BIG_Y, fitFont(b, 300),
          s.pl >= 0 ? Cfg::GREEN : Cfg::RED, Cfg::BG, false, b);

  const double ypl = yearPl(s, cy);
  txtFmt(S_RYRV, Cfg::R_C1_X + 9, Cfg::R_C1_Y + 22, F4,
         isNum(ypl) ? (ypl >= 0 ? Cfg::GREEN : Cfg::RED) : Cfg::MUTED,
         Cfg::CARD, false, "%s", numBuf(0, ypl, true));

  // lifetime = unrealised + realised (computed once in appRecompute)
  const double lt = g_app.summary.lifetimePl;
  txtFmt(S_RLTV, Cfg::R_C2_X + 9, Cfg::R_C2_Y + 22, F4,
         isNum(lt) ? (lt >= 0 ? Cfg::GREEN : Cfg::RED) : Cfg::MUTED,
         Cfg::CARD, false, "%s", numBuf(1, lt, true));

  txtFmt(S_RSALE, Cfg::R_C3_X + 9, Cfg::R_C3_Y + 15, F4, Cfg::TEXT, Cfg::CARD,
         false, "%u", (unsigned)s.count);
  txtFmt(S_RREV, Cfg::R_C4_X + 9, Cfg::R_C4_Y + 15, F4,
         s.reversed ? Cfg::YELLOW : Cfg::TEXT, Cfg::CARD, false, "%u",
         (unsigned)s.reversed);

  if (s.hasLast) {
    char lab[40];
    snprintf(lab, sizeof(lab), "LAST SALE %s", s.lastId);
    txtSlot(S_RLS1, 10, Cfg::R_LAST_Y + 1, F2, Cfg::MUTED, Cfg::BG, false, lab);
    dateText(b, sizeof(b), s.lastDate);
    txtSlot(S_RLS2, 310, Cfg::R_LAST_Y + 1, F2, Cfg::MUTED, Cfg::BG, true, b);
    txtFmt(S_RLS3, 10, Cfg::R_LAST_Y + 13, F4, Cfg::TEXT, Cfg::BG, false,
           "%.12s", s.lastSymbol);
    txtFmt(S_RLS4, 310, Cfg::R_LAST_Y + 13, F4,
           s.lastRealised >= 0 ? Cfg::GREEN : Cfg::RED, Cfg::BG, true, "%s",
           numBuf(2, s.lastRealised, true));
  }
}

// ---- chart ---------------------------------------------------------------
char g_pbtnNames[3][8] = {"1D", "5D", "1M"};

void drawPeriodButtons() {
  const int act = curRange();
  const int held = Touch::pressPeriod();
  useFont(F4);
  for (int i = 0; i < 3; i++) {
    const int x = Cfg::PBTN_X + i * (Cfg::PBTN_W + 1);
    const bool a = act == i;
    uint16_t fill = a ? Cfg::BLUE : Cfg::CARD;
    if (held == i) fill = blend565(Cfg::TEXT, a ? Cfg::BLUE : Cfg::CARD, 70);
    tft.fillRoundRect(x, Cfg::PBTN_Y, Cfg::PBTN_W, Cfg::PBTN_H, 8, fill);
    tft.drawRoundRect(x, Cfg::PBTN_Y, Cfg::PBTN_W, Cfg::PBTN_H, 8,
                      a ? Cfg::CYAN : Cfg::BORDER);
    const int tw = (int)tft.textWidth(g_pbtnNames[i]);
    tft.setTextColor(a || held == i ? Cfg::TEXT : Cfg::SECTXT, fill);
    tft.drawString(g_pbtnNames[i], x + (Cfg::PBTN_W - tw) / 2,
                   Cfg::PBTN_Y + (Cfg::PBTN_H - 16) / 2, 4);
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
  float mn = 1e30f, mx = -1e30f;
  for (int i = 0; i < n; i++) {
    if (c.v[i] < mn) mn = c.v[i];
    if (c.v[i] > mx) mx = c.v[i];
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
    tft.drawString(lb, Cfg::PLOT_X - 3 - w, y - 4, 1);
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

  const uint16_t col = c.v[n - 1] >= c.v[0] ? Cfg::GREEN : Cfg::RED;
  const uint16_t dim = blend565(col, Cfg::BG, 46);
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

uint32_t plotSig() {
  const Chart& c = g_app.charts[curRange()];
  return (uint32_t)curRange() * 7u + c.n * 131u + c.okEpoch;
}

// page rotation skips screens that have nothing to show (matches the
// SmallTV ViewSchedule rule: the realised page only exists once a sale stands)
bool viewEnabled(int s) {
  if (s == UI::SC_POS) return g_app.posCount > 0;
  if (s == UI::SC_REAL) return g_app.realised.valid && g_app.realised.count > 0;
  return true;
}

}  // namespace

namespace UI {

void begin() {
  tft.fillScreen(BG);
  drawDashStatics();
  drawHeader();
  drawNav();
  drawDots();
  if (g_range < 0) g_range = g_app.settings.defaultRange;
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
  g_badgeLast = (MarketStatus)-1;  // content area was just wiped
  tft.fillRect(0, Cfg::CONTENT_Y, Cfg::SCR_W, Cfg::CONTENT_H, BG);
  if (s == SC_DASH) drawDashStatics();
  else if (s == SC_POS) drawPosStatics();
  else if (s == SC_REAL) drawRealStatics();
  else drawPeriodButtons();
  drawHeader();
  drawDots();
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
  drawDots();
  drawNav();
}

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
    case Touch::A_SCREEN:
      if (arg == -1 || arg == +1) setScreen(g_curScreen + arg);  // swipe
      else setScreen(arg);                                      // nav zone / dot
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
      if (viewEnabled(t)) { setScreen(t); break; }
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
  txtSlot(S_TIME, 284, 8, F4, Cfg::TEXT, Cfg::HEADER, true, lt);

  switch (scr) {
    case SC_DASH:
      txtSlot(S_TITLE, 30, 8, F4, Cfg::TEXT, Cfg::HEADER, false,
              "S&P 500 Portfolio");
      drawDashDynamic();
      break;
    case SC_POS: {
      char t[24];
      snprintf(t, sizeof(t), "Positions (%u)", (unsigned)g_app.posCount);
      txtSlot(S_TITLE, 30, 8, F4, Cfg::TEXT, Cfg::HEADER, false, t);
      drawPosDynamic();
      break;
    }
    case SC_REAL:
      txtSlot(S_TITLE, 30, 8, F4, Cfg::TEXT, Cfg::HEADER, false,
              "Realised P/L");
      drawRealDynamic();
      break;
    default: {
      txtSlot(S_TITLE, 30, 8, F4, Cfg::TEXT, Cfg::HEADER, false,
              "Market Chart");
      const QuoteView& qv = g_app.quote;
      if (qv.valid) {
        txtFmt(S_CPX, 7, Cfg::MK_Y, FG18, Cfg::TEXT, Cfg::BG, false, "%s",
               numBuf(0, qv.last));
        useFont(FG18);
        const int pw = (int)tft.textWidth(numBuf(0, qv.last));
        if (isNum(qv.change))
          txtFmt(S_CCHG, 7 + pw + 12, Cfg::MK_Y + 6, F4,
                 qv.change >= 0 ? Cfg::GREEN : Cfg::RED, Cfg::BG, false,
                 "%s (%+.2f%%)", numBuf(1, qv.change, true), qv.changePct);
        else
          txtSlot(S_CCHG, 7 + pw + 12, Cfg::MK_Y + 6, F4, Cfg::MUTED, Cfg::BG,
                  false, "");
      } else {
        txtSlot(S_CPX, 7, Cfg::MK_Y, FG18, Cfg::MUTED, Cfg::BG, false, "--");
        txtSlot(S_CCHG, 7 + 60, Cfg::MK_Y + 6, F4, Cfg::MUTED, Cfg::BG, false,
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
