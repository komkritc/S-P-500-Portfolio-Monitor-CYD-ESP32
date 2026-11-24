/*
 * S&P 500 Portfolio Monitor for the ESP32 CYD (ESP32-2432S028R, 320x240
 * ILI9341 landscape + XPT2046 touch).
 *
 *   Yahoo Finance -> MarketData -> AppState -> FinancialCalculator -> LCD + Web
 *
 * Structure follows the ESP8266 SmallTV firmware (SP500_SmallTV.ino): everything
 * runs cooperatively from loop(), one Yahoo request at a time, Wi-Fi scan /
 * connect / reconnect are state machines. Display is TFT_eSPI (see
 * platformio.ini build flags), UI in src/DisplayUI.cpp.
 */
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <time.h>

#include "src/core/AppState.h"
#include "src/core/Format.h"
#include "src/core/MarketClock.h"
#include "src/core/PlatformShim.h"
#include "src/storage/StorageManager.h"
#include "src/storage/SaleStore.h"
#include "src/finance/Portfolio.h"
#include "src/finance/MarketData.h"
#include "src/network/WiFiManager.h"
#include "src/network/WebServerManager.h"
#include "src/network/OTAManager.h"
#include "src/TouchUI.h"
#include "src/DisplayUI.h"

TFT_eSPI tft = TFT_eSPI();  // pins come from the build flags (User_Setup off)

#define HEAP(tag) \
  Serial.printf("[heap] %-10s free=%u block=%u\n", tag, (unsigned)ESP.getFreeHeap(), (unsigned)maxFreeBlock())

static uint32_t s_lastHeapLog = 0;

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("[sys] S&P 500 Portfolio Monitor (ESP32 CYD) fw " FW_VERSION));

  HEAP("boot");
  appInit(g_app);

  // display first: a dark screen means the TFT init never completed
  tft.init();
  tft.setRotation(1);  // 320x240 landscape
  tft.fillScreen(TFT_BLACK);
  tft.setSwapBytes(true);
  UI::bootBanner(5, "boot");
  HEAP("display");

  UI::bootBanner(20, "storage");
  storage::begin();
  HEAP("littlefs");
  storage::loadSettings(g_app.settings);
  g_app.posCount = storage::loadPortfolio(g_app);
  storage::ledgerBoot(g_app);  // replay /sells.jsonl into positions + realised totals
  bookSync(g_app.book, g_app.positions, g_app.posCount);
  // The SP500 quote slot always exists (detailed quote + '^GSPC' ticker),
  // even with an empty portfolio.
  if (!bookFind(g_app.book, SP500_SYMBOL)) {
    SymQuote& q = g_app.book.q[g_app.book.n++];
    memset(&q, 0, sizeof q);
    strcpy(q.symbol, SP500_SYMBOL);
    q.price = NAN;
  }
  g_app.effPortfolioS = g_app.settings.portfolioRefreshS;
  g_app.effQuoteS = g_app.settings.quoteRefreshS;
  g_app.effChartS = g_app.settings.chartRefreshS;
  bool cached = storage::loadCache(g_app);  // reboot offline still shows last good data
  Serial.printf("[sys] %u position(s), cache %s, heap %u\n", g_app.posCount,
                cached ? "loaded" : "empty", (unsigned)ESP.getFreeHeap());

  UI::bootBanner(45, "wifi");
  wifimgr::begin(g_app.settings.hostname);
  HEAP("wifi");
  market::begin();
  ota::begin(g_app.settings.hostname, g_app.settings.adminPass);
  HEAP("ota");
  webserver::begin();
  HEAP("web");

  UI::bootBanner(75, "touch");
  Touch::begin();

  UI::begin();  // first full frame: chrome + dashboard statics
  Serial.printf("[sys] ready, heap %u, max block %u\n", (unsigned)ESP.getFreeHeap(),
                (unsigned)maxFreeBlock());
}

void loop() {
  wifimgr::loop();
  webserver::loop();
  ota::loop();
  ota::restartLoop();
  if (ota::active()) return;  // nothing else may touch flash/RAM during an update

  market::loop();

  // One recompute per loop: the only place totals are calculated. LCD and web
  // read the results.
  time_t t = time(nullptr);
  appRecompute(g_app, millis(), t > 1600000000L ? (uint32_t)t : 0);
  wifimgr::fillNetInfo(g_app.net);
  UI::update();

  uint32_t now = millis();
  static uint32_t lastData = 0;
  if (now - lastData >= 20000UL) {  // serial mirror of what the LCD/web show
    lastData = now;
    char b1[32], b2[32], b3[32];
    fmtThb(b1, sizeof b1, g_app.summary.nav);
    fmtThbSigned(b2, sizeof b2, g_app.summary.pl);
    fmtPct(b3, sizeof b3, g_app.summary.retPct);
    Serial.printf("[data] NAV %s  P/L %s (%s)  pos=%u priced=%u stale=%d | ", b1, b2, b3,
                  g_app.posCount, g_app.summary.priced, (int)g_app.summary.stale);
    fmtIndex(b1, sizeof b1, g_app.quote.last);
    fmtIndexSigned(b2, sizeof b2, g_app.quote.change);
    fmtPct(b3, sizeof b3, g_app.quote.changePct);
    Serial.printf("SPX %s %s (%s) %s stale=%d | charts %u/%u/%u\n", b1, b2, b3,
                  g_app.quote.status == MS_OPEN ? "OPEN" : g_app.quote.status == MS_PRE ? "PRE"
                  : g_app.quote.status == MS_CLOSED ? "CLOSED" : "N/A",
                  (int)g_app.quote.stale, g_app.charts[0].n, g_app.charts[1].n, g_app.charts[2].n);
  }
  if (now - s_lastHeapLog >= 300000UL) {
    s_lastHeapLog = now;
    Serial.printf("[sys] heap free=%u block=%u frag=%u%% up=%lus\n", (unsigned)ESP.getFreeHeap(),
                  (unsigned)maxFreeBlock(), (unsigned)heapFragPct(), (unsigned long)(now / 1000));
  }
  delay(2);  // ESP32: short yield keeps the WDT happy without hogging a core
}
