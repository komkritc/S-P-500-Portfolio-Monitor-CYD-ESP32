#include "YahooParser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void YahooParser::begin(uint32_t *tsBuf, float *closeBuf, float *oBuf, float *hBuf, float *lBuf, uint16_t cap) {
  price = prevClose = dayHigh = dayLow = open = NAN;
  prevClose2_ = NAN;
  marketTime = 0; gmtOffset = 0; count = 0;
  tz[0] = 0; symbol[0] = 0; tzName[0] = 0; errCode_[0] = 0; err_[0] = 0;
  ts_ = tsBuf; cl_ = closeBuf; oh_ = oBuf; hh_ = hBuf; lh_ = lBuf; cap_ = cap;
  tsN_ = clN_ = 0;
  overflow_ = resultNull_ = haveResult_ = haveOpen_ = false;
  if (cl_) for (uint16_t i = 0; i < cap_; i++) cl_[i] = NAN;
  if (oh_) for (uint16_t i = 0; i < cap_; i++) oh_[i] = NAN;
  if (hh_) for (uint16_t i = 0; i < cap_; i++) hh_[i] = NAN;
  if (lh_) for (uint16_t i = 0; i < cap_; i++) lh_[i] = NAN;
  js_.begin(&YahooParser::onEvent, this);
}

void YahooParser::onEvent(void *ctx, const char *path, int idx, JsonKind k, const char *text) {
  static_cast<YahooParser *>(ctx)->handle(path, idx, k, text);
}

static double num(const char *s) { return strtod(s, nullptr); }

// Market sanity: no real quote is >= $10^10; anything outside (0, PX_MAX) is malformed or
// adversarial input and becomes NaN ("unknown"), which every downstream consumer must handle.
static const double PX_MAX = 1e10;
static inline double saneNum(double v) { return (isNum(v) && v > 0 && v < PX_MAX) ? v : NAN; }

void YahooParser::handle(const char *path, int idx, JsonKind k, const char *text) {
  static const char RES[] = "chart.result[]";
  if (strncmp(path, "chart.error", 11) == 0) {
    if (k == JK_STR && strcmp(path, "chart.error.code") == 0) { strncpy(errCode_, text, sizeof errCode_ - 1); errCode_[sizeof errCode_ - 1] = 0; }
    return;
  }
  if (strcmp(path, "chart.result") == 0) { if (k == JK_NULL) resultNull_ = true; return; }
  if (strncmp(path, RES, sizeof RES - 1) != 0) return;
  haveResult_ = true;
  const char *p = path + sizeof RES - 1;
  if (strncmp(p, ".meta.", 6) == 0) {
    p += 6;
    if (k == JK_NUM) {
      if (!strcmp(p, "regularMarketPrice")) price = saneNum(num(text));
      else if (!strcmp(p, "regularMarketTime")) marketTime = (uint32_t)strtoul(text, nullptr, 10);
      else if (!strcmp(p, "chartPreviousClose")) prevClose = saneNum(num(text));
      else if (!strcmp(p, "previousClose")) prevClose2_ = saneNum(num(text));
      else if (!strcmp(p, "regularMarketDayHigh")) dayHigh = saneNum(num(text));
      else if (!strcmp(p, "regularMarketDayLow")) dayLow = saneNum(num(text));
      else if (!strcmp(p, "gmtoffset")) gmtOffset = (int32_t)strtol(text, nullptr, 10);
    } else if (k == JK_STR) {
      if (!strcmp(p, "timezone")) { strncpy(tz, text, sizeof tz - 1); tz[sizeof tz - 1] = 0; }
      else if (!strcmp(p, "exchangeTimezoneName")) { strncpy(tzName, text, sizeof tzName - 1); tzName[sizeof tzName - 1] = 0; }
      else if (!strcmp(p, "symbol")) { strncpy(symbol, text, sizeof symbol - 1); symbol[sizeof symbol - 1] = 0; }
    }
    return;
  }
  if (idx < 0) return;
  if (!strcmp(p, ".timestamp[]")) {
    if (!ts_) return;
    if ((uint16_t)idx >= cap_) { overflow_ = true; return; }
    if (k == JK_NUM) { ts_[idx] = (uint32_t)strtoul(text, nullptr, 10); if ((uint16_t)(idx + 1) > tsN_) tsN_ = (uint16_t)(idx + 1); }
  } else if (!strcmp(p, ".indicators.quote[].close[]")) {
    if (!cl_) return;
    if ((uint16_t)idx >= cap_) { overflow_ = true; return; }
    if ((uint16_t)(idx + 1) > clN_) clN_ = (uint16_t)(idx + 1);
    if (k == JK_NUM) cl_[idx] = (float)num(text);   // null stays NaN
  } else if (!strcmp(p, ".indicators.quote[].open[]")) {
    if (oh_) {
      if ((uint16_t)idx >= cap_) { overflow_ = true; return; }
      if (k == JK_NUM) oh_[idx] = (float)num(text);
    }
    if (k == JK_NUM && !haveOpen_) { open = num(text); haveOpen_ = true; }   // first bar of the range = session open
  } else if (!strcmp(p, ".indicators.quote[].high[]")) {
    if (!hh_) return;
    if ((uint16_t)idx >= cap_) { overflow_ = true; return; }
    if (k == JK_NUM) hh_[idx] = (float)num(text);
  } else if (!strcmp(p, ".indicators.quote[].low[]")) {
    if (!lh_) return;
    if ((uint16_t)idx >= cap_) { overflow_ = true; return; }
    if (k == JK_NUM) lh_[idx] = (float)num(text);
  }
}

bool YahooParser::finish() {
  if (js_.failed()) { snprintf(err_, sizeof err_, "bad JSON"); return false; }
  if (!js_.complete()) { snprintf(err_, sizeof err_, "truncated response"); return false; }
  if (errCode_[0]) { snprintf(err_, sizeof err_, "%.30s", errCode_); return false; }
  if (resultNull_ || !haveResult_) { snprintf(err_, sizeof err_, "no data"); return false; }
  if (overflow_) { snprintf(err_, sizeof err_, "too many points"); return false; }
  if (isNum(prevClose2_)) prevClose = prevClose2_;   // previousClose = prior session close; chartPreviousClose is the range start for 5d/1mo
  // compact the series: both ts and close must be present, timestamps strictly increasing
  count = 0;
  if (ts_ && cl_) {
    uint16_t n = tsN_ < clN_ ? tsN_ : clN_;
    uint32_t lastT = 0;
    for (uint16_t i = 0; i < n; i++) {
      if (!isNum(cl_[i]) || cl_[i] <= 0 || cl_[i] >= PX_MAX || ts_[i] <= lastT) continue;
      ts_[count] = ts_[i]; cl_[count] = cl_[i];
      if (oh_ && hh_ && lh_) {   // keep the OHLC bar consistent with the surviving close
        float o = oh_[i], h = hh_[i], l = lh_[i], c = cl_[count];
        if (!isNum(o) || !isNum(h) || !isNum(l) || h < l) {
          oh_[count] = hh_[count] = lh_[count] = c;   // degrade to a doji, invent nothing
        } else {
          if (o > h) h = o;
          if (c > h) h = c;              // clamp high around the body
          if (o < l) l = o;
          if (c < l) l = c;              // clamp low around the body
          oh_[count] = o; hh_[count] = h; lh_[count] = l;
        }
      }
      lastT = ts_[i]; count++;
    }
  }
  return true;
}
