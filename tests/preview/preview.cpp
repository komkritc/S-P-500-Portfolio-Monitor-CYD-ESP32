// Host render of the CYD's real LCD path: DisplayUI.cpp against the mock
// TFT_eSPI (real font tables, real draw algorithms), driven through the same
// UI::setScreen / UI::update sequence the device loop runs. Yahoo fixtures are
// fed through the real YahooParser. Checks: no out-of-bounds pixel writes, no
// overlapping text ink, every text inside the screen. Writes <dir>/preview/out/
// <name>.raw (RGB565 LE, 320x240) for raw2png.py.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <stdarg.h>
#include <string>
#include <vector>
#include <stdio.h>

#include "DisplayUI.h"
#include "TouchUI.h"
#include "config.h"
#include "core/AppState.h"
#include "core/MarketClock.h"
#include "finance/Portfolio.h"
#include "finance/ChartData.h"
#include "finance/YahooParser.h"
#include "finance/SaleEngine.h"
#include "finance/MarketData.h"

// ---- mock Arduino globals ----
uint32_t g_arduinoMs = 0;
void SerialMock::printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); }
void SerialMock::print(const char *s) { fputs(s, stderr); }
void SerialMock::print(int v) { fprintf(stderr, "%d", v); }
void SerialMock::println(const char *s) { fputs(s, stderr); fputc('\n', stderr); }
void SerialMock::println(int v) { fprintf(stderr, "%d\n", v); }
void SerialMock::println(long v) { fprintf(stderr, "%ld\n", v); }
SerialMock Serial;

// ---- UI collaborators not exercised on the host ----
TFT_eSPI tft;
namespace Touch {
void begin() {}
Act poll(int &) { return A_NONE; }
int pressZone() { return -1; }
int pressPeriod() { return -1; }
void setScreen(int) {}
}  // namespace Touch
namespace market {
void setViewRange(uint8_t) {}
}  // namespace market

// ---- scenario data (identical fixtures + builders to the SmallTV harness) ----
static int g_problems = 0, g_frames = 0;
static std::string g_dir;
static const uint32_t NOW_CLOSED = 1795210600;  // Fri 2026-11-20 16:36:40 ET: market closed, quote = the 16:00 close
static const uint32_t NOW_PRE   = 1795179600;  // Fri 2026-11-20 08:00 ET (13:00 UTC): inside the 04:00-09:30 ET pre-market window

static std::string readFile(const std::string &p) {
  FILE *f = fopen(p.c_str(), "rb");
  if (!f) { printf("cannot open %s\n", p.c_str()); exit(2); }
  std::string s; char b[4096]; size_t n;
  while ((n = fread(b, 1, sizeof b, f))) s.append(b, n);
  fclose(f);
  return s;
}

static void loadCharts(AppState &a, uint32_t nowMs, uint32_t epoch) {
  static const char *files[3] = {"c1d", "c5d", "c1m"};
  for (int r = 0; r < 3; r++) {
    std::string body = readFile(g_dir + "/fixtures/" + files[r] + ".json");
    static uint32_t ts[STAGE_CAP]; static float cl[STAGE_CAP];
    static float oh[STAGE_CAP], hh[STAGE_CAP], lh[STAGE_CAP];   // candle bars, as the device stages them
    YahooParser p; p.begin(ts, cl, oh, hh, lh, STAGE_CAP); p.feed(body.data(), body.size()); p.finish();
    float base = r == RANGE_1D ? (float)p.prevClose : NAN;
    chartStore(a.charts[r], ts, oh, hh, lh, cl, p.count, base, nowMs, epoch);
  }
}

static void loadQuote(AppState &a, uint32_t nowMs, uint32_t epoch) {
  std::string body = readFile(g_dir + "/fixtures/q.json");
  YahooParser p; p.begin(nullptr, nullptr, nullptr, nullptr, nullptr, 0); p.feed(body.data(), body.size()); p.finish();
  SP500Quote &q = a.sp; memset(&q, 0, sizeof q);
  q.valid = true; q.last = p.price; q.prevClose = p.prevClose; q.open = p.open; q.high = p.dayHigh; q.low = p.dayLow;
  q.marketTime = p.marketTime; strcpy(q.tz, p.tz); q.okMs = nowMs; q.okEpoch = epoch;
  SymQuote *b = bookFind(a.book, SP500_SYMBOL);
  if (b) { b->valid = true; b->price = p.price; b->marketTime = p.marketTime; b->okMs = nowMs; b->okEpoch = epoch; }
}

static void addPos(AppState &a, const char *sym, const char *date, double qty, double buy) {
  portfolioAdd(a.positions, a.posCount, a.pmeta, sym, date, qty, buy, 0);
}

enum Scn { S_NORMAL, S_LOSS, S_MANY, S_MISSING, S_STALE, S_EMPTY, S_NODATA, S_BIG, S_REALISED, S_REALBIG, S_UP, S_PRE };

static void addSale(AppState &a, const char *id, const char *date, double realised) {
  SaleRecord r; memset(&r, 0, sizeof r);
  r.seq = a.realised.records + 1; r.ts = NOW_CLOSED; r.type = REC_SALE;
  strncpy(r.id, id, sizeof r.id - 1); strncpy(r.symbol, "^GSPC", sizeof r.symbol - 1);
  strncpy(r.date, date, sizeof r.date - 1);
  r.realised = realised; r.nAlloc = 1; r.alloc[0].lot = 1;
  sale::summaryAdd(a.realised, r);
}

static void addReverse(AppState &a, const char *id, const char *refId, const char *date, double realised) {
  SaleRecord r; memset(&r, 0, sizeof r);
  r.seq = a.realised.records + 1; r.ts = NOW_CLOSED; r.type = REC_REVERSAL;
  strncpy(r.id, id, sizeof r.id - 1); strncpy(r.refId, refId, sizeof r.refId - 1);
  strncpy(r.symbol, "^GSPC", sizeof r.symbol - 1); strncpy(r.date, date, sizeof r.date - 1);
  r.realised = -realised; r.nAlloc = 1; r.alloc[0].lot = 1;
  sale::summaryAdd(a.realised, r);
}

static void build(AppState &a, Scn s, uint32_t epoch) {
  appInit(a);
  // the device runs storage::ledgerBoot() before the first frame: recover() calls
  // summaryReset(), so an empty ledger is valid ("no sales yet"), never "unreadable"
  sale::summaryReset(a.realised);
  a.net.mode = NetInfo::NET_STA; a.net.connected = s != S_NODATA; a.net.rssi = -58;
  strcpy(a.net.ip, "192.168.1.50"); strcpy(a.net.ssid, "home");
  a.settings.defaultRange = 0;
  const uint32_t nowMs = g_arduinoMs;
  if (s == S_EMPTY) { /* no positions */ }
  else if (s == S_MANY) {
    addPos(a, "^GSPC", "2025-11-20", 39.17, 6382.10); addPos(a, "^GSPC", "2026-01-05", 12.5, 7010.5); addPos(a, "^GSPC", "2026-02-02", 3.25, 7300);
    addPos(a, "^GSPC", "2026-03-02", 1.1, 6100); addPos(a, "^GSPC", "2026-04-01", 100.123456, 6900); addPos(a, "^GSPC", "2026-05-01", 0.5, 7800); addPos(a, "^GSPC", "2026-06-01", 2, 6500);
    // a lot sized like the real portfolio's invested capital (about THB 10.6 M
    // in total), so the whole-baht INVESTED number is checked at that scale
    addPos(a, "^GSPC", "2026-07-01", 1400, 6800);
  } else if (s == S_REALISED) {
    addPos(a, "^GSPC", "2025-11-20", 39.17, 6382.10); addPos(a, "^GSPC", "2026-01-05", 12.5, 7010.5); addPos(a, "^GSPC", "2026-02-02", 3.25, 6200);
    sale::summaryReset(a.realised);
    addSale(a, "TX-000001", "2026-10-15", 2450.75);
    addSale(a, "TX-000002", "2025-06-01", -800.25);
    addReverse(a, "TX-000003", "TX-000002", "2025-06-01", -800.25);
    a.realised.hasLast = true; strcpy(a.realised.lastId, "TX-000001"); strcpy(a.realised.lastSymbol, "^GSPC");
    strcpy(a.realised.lastDate, "2026-10-15"); a.realised.lastRealised = 2450.75; a.realised.lastTs = NOW_CLOSED;
  } else if (s == S_REALBIG) {
    addPos(a, "^GSPC", "2025-11-20", 39.17, 6382.10);
    sale::summaryReset(a.realised);
    addSale(a, "TX-000001", "2026-11-19", -1234567.89);
    a.realised.hasLast = true; strcpy(a.realised.lastId, "TX-000001"); strcpy(a.realised.lastSymbol, "^GSPC");
    strcpy(a.realised.lastDate, "2026-11-19"); a.realised.lastRealised = -1234567.89; a.realised.lastTs = NOW_CLOSED;
  } else if (s == S_BIG) { addPos(a, "^GSPC", "2025-11-20", 1234567.891, 6382.10); }
  else if (s == S_LOSS) { addPos(a, "^GSPC", "2026-02-02", 20, 7300); addPos(a, "^GSPC", "2026-03-03", 5, 7500); }
  else { addPos(a, "^GSPC", "2025-11-20", 39.17, 6382.10); addPos(a, "^GSPC", "2026-01-05", 12.5, 7010.5); addPos(a, "^GSPC", "2026-02-02", 3.25, 6200); }
  bookSync(a.book, a.positions, a.posCount);
  if (!bookFind(a.book, SP500_SYMBOL)) {
    SymQuote &q = a.book.q[a.book.n++]; memset(&q, 0, sizeof q);
    strcpy(q.symbol, SP500_SYMBOL); q.price = NAN;
  }
  if (s == S_NODATA) { a.sp.last = a.sp.prevClose = a.sp.open = a.sp.high = a.sp.low = NAN; }
  else {
    loadQuote(a, nowMs, epoch);
    if (s == S_MISSING) { SymQuote *q = bookFind(a.book, SP500_SYMBOL); q->valid = false; q->price = NAN; }
    loadCharts(a, nowMs, epoch);
  }
  if (s == S_STALE) { a.sp.failed = true; for (int i = 0; i < a.book.n; i++) a.book.q[i].failed = true; for (auto &c : a.charts) c.failed = true; }
  if (s == S_UP) {
    // a rising period: the plot line and its fill must be GREEN for all three
    // ranges (the fixtures are all falling, so they only cover the red path).
    // Reverse the whole bar, not just the close: a candle whose open/high/low come from a
    // different bar than its close is pure noise (it drew a fan of giant wicks before).
    // Timestamps stay put, so the x axis and the labels are untouched.
    for (auto &c : a.charts) {
      for (uint16_t i = 0, j = c.n ? c.n - 1 : 0; i < j; i++, j--) {
        float t2 = c.v[i]; c.v[i] = c.v[j]; c.v[j] = t2;
        t2 = c.o[i]; c.o[i] = c.o[j]; c.o[j] = t2;
        t2 = c.h[i]; c.h[i] = c.h[j]; c.h[j] = t2;
        t2 = c.l[i]; c.l[i] = c.l[j]; c.l[j] = t2;
      }
      c.base = c.v[0] * 0.98f;              // reference below the close -> change > 0
    }
    a.sp.last = a.sp.prevClose * 1.02;      // header change also positive
    SymQuote *b = bookFind(a.book, SP500_SYMBOL);
    if (b) b->price = a.sp.last;
  }
  appRecompute(a, nowMs, epoch);
}

static void checkText(const char *name) {
  const std::vector<TxtBox> &v = tft.boxes;
  if (getenv("DUMPBOXES"))
    for (auto &bb : v) printf("BOX %s \"%s\" (%d,%d %dx%d)\n", name, bb.s, bb.x, bb.y, bb.w, bb.h);
  for (size_t i = 0; i < v.size(); i++) {
    const TxtBox &a = v[i];
    if (a.x < 0 || a.y < 0 || a.x + a.w > Cfg::SCR_W || a.y + a.h > Cfg::SCR_H) {
      printf("  PROBLEM %s: text \"%s\" ink outside screen (%d,%d %dx%d)\n", name, a.s, a.x, a.y, a.w, a.h);
      g_problems++;
    }
    for (size_t j = i + 1; j < v.size(); j++) {
      const TxtBox &b = v[j];
      if (a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h && !strcmp(a.s, b.s)) continue;  // identical repaint
      bool ov = a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
      if (ov) {
        int ix0 = a.x > b.x ? a.x : b.x, ix1 = (a.x+a.w < b.x+b.w ? a.x+a.w : b.x+b.w) - ix0;
        int iy0 = a.y > b.y ? a.y : b.y, iy1 = (a.y+a.h < b.y+b.h ? a.y+a.h : b.y+b.h) - iy0;
        printf("  PROBLEM %s: \"%s\" (%d,%d %dx%d) overlaps \"%s\" (%d,%d %dx%d) intr=%dx%d@%d,%d\n",
               name, a.s, a.x, a.y, a.w, a.h, b.s, b.x, b.y, b.w, b.h, ix1, iy1, ix0, iy0);
        g_problems++;
      }
    }
  }
}

static void dump(const char *name) {
  std::string p = g_dir + "/preview/out/" + name + ".raw";
  FILE *f = fopen(p.c_str(), "wb");
  fwrite(tft.fb, 2, Cfg::SCR_W * Cfg::SCR_H, f);
  fclose(f);
  printf("%-28s oob=%ld", name, tft.oob);
  if (tft.oob) printf("  first at (%d,%d)", tft.oobX, tft.oobY);
  printf("\n");
  if (tft.oob) g_problems++;
  checkText(name);
  tft.boxes.clear();
  g_frames++;
}

// Device-like frame: full repaint through invalidate() + two 4 Hz updates so
// every slot and badge in the cache has been drawn once.
static void showFrame(const char *name) {
  UI::invalidate();
  for (int k = 0; k < 2; k++) { g_arduinoMs += 500; UI::update(); }
  tft.oob = 0; tft.oobX = tft.oobY = 0;
  dump(name);
}

static const char *rangeLabel(int r) { return r == 0 ? "1d" : r == 1 ? "5d" : "1mo"; }

int main(int argc, char **argv) {
  g_dir = argc > 1 ? argv[1] : ".";
  AppState &a = g_app;  // DisplayUI draws from the global; build fixtures into it

  UI::bootBanner(60, "Loading portfolio...");
  dump("boot_banner");

  struct { Scn s; const char *n; uint32_t ep; } scn[] = {
    {S_NORMAL, "normal", NOW_CLOSED}, {S_LOSS, "loss", NOW_CLOSED}, {S_MANY, "many", NOW_CLOSED},
    {S_MISSING, "missing", NOW_CLOSED}, {S_STALE, "stale", NOW_CLOSED}, {S_EMPTY, "empty", NOW_CLOSED},
    {S_NODATA, "nodata", NOW_CLOSED}, {S_BIG, "bignum", NOW_CLOSED},
    {S_REALISED, "realised", NOW_CLOSED}, {S_REALBIG, "realbig", NOW_CLOSED}, {S_UP, "up", NOW_CLOSED},
    // pre-market clock: the dashboard must show NO status chip, while the Market page
    // still shows the yellow PRE-MKT badge (fw 1.3.2 dashboard simplification).
    {S_PRE, "pre", NOW_PRE}};
  char n[64];
  for (auto &sc : scn) {
    build(a, sc.s == S_PRE ? S_NORMAL : sc.s, sc.ep);

    UI::setScreen(UI::SC_DASH);
    snprintf(n, sizeof n, "%s_p1_dashboard", sc.n); showFrame(n);

    const int pages = a.posCount ? (a.posCount + Cfg::TBL_ROWS - 1) / Cfg::TBL_ROWS : 1;
    UI::setScreen(UI::SC_CHART);            // nav order: Dashboard | Chart | Positions | Realised
    for (int r = 0; r < 3; r++) {
      UI::setRange(r);
      snprintf(n, sizeof n, "%s_p2_chart_%s", sc.n, rangeLabel(r)); showFrame(n);
    }

    UI::setScreen(UI::SC_POS);
    for (int w = 0; w < pages; w++) {
      if (w) { UI::scrollPos(1); for (int k = 0; k < 2; k++) { g_arduinoMs += 500; UI::update(); } }
      snprintf(n, sizeof n, "%s_p3_positions_w%d", sc.n, w); showFrame(n);
    }

    UI::setScreen(UI::SC_REAL);
    snprintf(n, sizeof n, "%s_p4_realised", sc.n); showFrame(n);
  }

  // Bollinger Bands on the LCD Market chart (fw 1.1.8, web Settings -> "BB on the LCD chart").
  // One frame per range; the 1M frame asks for SMA 50 so the n/2 clamp is visible in its corner
  // tag, and a final frame with the switch off proves the setting really drives the plot.
  build(a, S_NORMAL, NOW_CLOSED);
  UI::setScreen(UI::SC_CHART);
  a.settings.bbLcd = 1;
  const int bbp[3] = {10, 20, 50};
  static uint16_t bbOn[Cfg::SCR_W * Cfg::SCR_H];
  for (int r = 0; r < 3; r++) {
    a.settings.bbLcdP = (uint8_t)bbp[r];
    UI::setRange(r);
    snprintf(n, sizeof n, "bbon_chart_%s_p%d", rangeLabel(r), bbp[r]);
    showFrame(n);
    if (r == 2) memcpy(bbOn, tft.fb, sizeof bbOn);
  }
  a.settings.bbLcd = 0;
  a.settings.bbLcdP = 20;
  showFrame("bboff_chart_1mo");
  { size_t green = 0, red = 0, diff = 0;
    for (int y = Cfg::PLOT_Y; y <= Cfg::PLOT_Y + Cfg::PLOT_H; y++)
      for (int x = Cfg::PLOT_X; x <= Cfg::PLOT_X + Cfg::PLOT_W; x++)
        if (bbOn[y * Cfg::SCR_W + x] != tft.fb[y * Cfg::SCR_W + x]) diff++;
    if (diff < 40) { printf("  PROBLEM bb: bands not drawn (%zu plot pixels differ)\n", diff); g_problems++; }
    else printf("bb: %zu plot pixels drawn by the bands + candles\n", diff);
    // Candlesticks are per-bar coloured, so a real bar chart shows both directions in the same
    // frame; the plain line chart is a single colour and would leave one of these at zero.
    for (int y = Cfg::PLOT_Y; y <= Cfg::PLOT_Y + Cfg::PLOT_H; y++)
      for (int x = Cfg::PLOT_X; x <= Cfg::PLOT_X + Cfg::PLOT_W; x++) {
        const uint16_t p = bbOn[y * Cfg::SCR_W + x];
        if (p == Cfg::GREEN) green++; else if (p == Cfg::RED) red++;
      }
    if (green < 20 || red < 20) {
      printf("  PROBLEM bb: candles not drawn (green %zu red %zu)\n", green, red); g_problems++;
    } else printf("bb: candles %zu green / %zu red bar pixels\n", green, red);
  }

  // the same bands over an up day: the README keeps one chart image per direction, and both
  // should show what the panel draws with the switch on (candles + bands), not the old line.
  build(a, S_UP, NOW_CLOSED);
  UI::setScreen(UI::SC_CHART);
  a.settings.bbLcd = 1;
  a.settings.bbLcdP = 20;
  UI::setRange(RANGE_5D);
  showFrame("bbup_chart_5d_p20");
  a.settings.bbLcd = 0;                    // back to the shipped default for the frames below

  // incremental path: change a quote and let the 4 Hz cache repaint without a
  // full invalidate; the result must match a fresh render of the same state.
  build(a, S_NORMAL, NOW_CLOSED);
  UI::setScreen(UI::SC_DASH);
  showFrame("inc_base");
  SymQuote *q = bookFind(a.book, SP500_SYMBOL);
  q->price = 6000; a.sp.last = 6000;
  appRecompute(a, g_arduinoMs, NOW_CLOSED);
  for (int k = 0; k < 3; k++) { g_arduinoMs += 500; UI::update(); }
  static uint16_t inc[Cfg::SCR_W * Cfg::SCR_H];
  memcpy(inc, tft.fb, sizeof inc);
  {
    std::string p = g_dir + "/preview/out/inc_cached.raw";
    FILE *f = fopen(p.c_str(), "wb");
    fwrite(inc, 2, Cfg::SCR_W * Cfg::SCR_H, f);
    fclose(f);
  }
  tft.oob = 0;
  if (tft.oob) { printf("  PROBLEM inc: oob after repaint\n"); g_problems++; }
  build(a, S_NORMAL, NOW_CLOSED);
  q = bookFind(a.book, SP500_SYMBOL); q->price = 6000; a.sp.last = 6000;
  appRecompute(a, g_arduinoMs, NOW_CLOSED);
  showFrame("inc_fresh");
  if (memcmp(inc, tft.fb, sizeof inc) != 0) { printf("  PROBLEM inc: cached repaint != fresh render\n"); g_problems++; }

  printf("%d frames, %d problems\n", g_frames, g_problems);
  return g_problems ? 1 : 0;
}
