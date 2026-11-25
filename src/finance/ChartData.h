// ChartData.h - chart storage helpers (pure, host-testable).
#pragma once
#include "../core/AppTypes.h"

// Min/max-preserving downsample to at most maxOut points (keeps the first and last point and the
// extremes of every bucket, in time order). Never invents points. Returns the number written.
uint16_t downsampleMinMax(const uint32_t *t, const float *v, uint16_t n, uint32_t *outT, float *outV, uint16_t maxOut);

// OHLC-preserving aggregation to at most maxOut bars: first and last bar verbatim, middle split
// into buckets where each bucket merges into one candle (open = first open, high = bucket max,
// low = bucket min, close = last close, ts = last ts). Never invents points.
uint16_t downsampleOHLC(const uint32_t *t, const float *o, const float *h, const float *l, const float *v,
                        uint16_t n, uint32_t *outT, float *outO, float *outH, float *outL, float *outV,
                        uint16_t maxOut);

// Copy staged points into a stored chart (aggregating when they do not fit its capacity).
void chartStore(Chart &c, const uint32_t *t, const float *o, const float *h, const float *l,
                const float *v, uint16_t n, float base, uint32_t okMs, uint32_t okEpoch);

// ---------------------------------------------------------------- Bollinger Bands
// The periods the settings accept, and the width on the panel (sigma). The web overlay lets the
// browser pick its own width; the LCD always draws BB_SIGMA because the plot is only 80 px tall.
static const uint8_t  BB_P_MIN = 10, BB_P_MAX = 50;
static const float    BB_SIGMA = 2.0f;

// Bollinger Bands over a close series: SMA(p) +/- BB_SIGMA * population sd of the same window.
// Exactly the running sum / sum-of-squares maths the web dashboard runs, so the panel and the
// browser agree to the last digit. mid/up/dn must hold n floats; the leading p-1 entries are NaN.
// Returns the number of complete bands, 0 when p < 2 or n < p (never touches nothing else).
// `v` must be finite - chartStore only ever stores parsed, non-null closes.
uint16_t bollinger(const float *v, uint16_t n, uint8_t p, float *mid, float *up, float *dn);

const char *rangeName(uint8_t r);        // "1d" "5d" "1mo"
const char *rangeLabel(uint8_t r);       // "1D" "5D" "1M"
const char *rangeInterval(uint8_t r);    // "5m" "15m" "1d"
bool parseRange(const char *s, uint8_t &r);

// Close price for the buy date (original sketch's fetchCloseOnDate rule, tightened): the bar whose
// exchange-local date equals y-m-d; otherwise the last bar BEFORE that date (weekend/holiday);
// a date after every bar therefore yields the newest bar. A date before every bar returns false
// (no price is invented). Returns false when no bar qualifies.
// `fixedOffsetSec` = exchange UTC offset for non-US symbols; EXCHANGE_NY (default) uses the New York DST rules.
static const int32_t EXCHANGE_NY = 0x7fffffff;
// When *exact is false the close was borrowed from that earlier session: `barTs` (optional) reports the
// bar that was used, and the caller MUST show the user its date instead of silently inventing one.
bool pickCloseOnDate(const uint32_t *t, const float *v, uint16_t n, int y, int m, int d, double &price, bool *exact = nullptr, int32_t fixedOffsetSec = EXCHANGE_NY, uint32_t *barTs = nullptr);

// "Nice" axis ticks (1/2/2.5/5 x 10^k) inside [lo, hi], at most maxN. Returns the count; ticks[] ascending.
// Pure, host-tested. A flat series (hi == lo) yields one tick at that value.
int niceTicks(double lo, double hi, double *ticks, int maxN, double *stepOut = nullptr);
