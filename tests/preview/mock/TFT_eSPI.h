// Host mock of TFT_eSPI for the CYD preview harness. Renders into a real
// 320x240 RGB565 framebuffer with the *same algorithms and font tables* the
// board uses (TFT_eSPI smooth-vlw AA, Font16 bitmap, Font32rle RLE, GLCD), so
// the produced PNGs match the device pixel-for-pixel where the layout is
// correct. Every plotted pixel is bounds-checked (oob counter) and every
// drawString records the ink bounding box of the glyphs for the overlap check.
//
// Include order matters: mock Arduino.h first (PROGMEM, pgm_read_*).
#pragma once
#include <Arduino.h>
#include <vector>
#include <string.h>
#include <math.h>
#include <stdio.h>

#ifdef PIXTRACE
static int g_pixOn = 0, g_pixX0 = 242, g_pixX1 = 242, g_pixY0 = 60, g_pixY1 = 85;
#define PIXCHK() do { if (g_pixOn && x <= g_pixX1 && x + w - 1 >= g_pixX0 && y >= g_pixY0 && y <= g_pixY1) fprintf(stderr, "PIXRUN x=%d y=%d w=%d c=%04x\n", (int)x, (int)y, (int)w, c); } while (0)
#define PIXONE() do { if (g_pixOn && x >= g_pixX0 && x <= g_pixX1 && y >= g_pixY0 && y <= g_pixY1) fprintf(stderr, "PIX x=%d y=%d c=%04x\n", (int)x, (int)y, c); } while (0)
#else
#define PIXCHK()
#define PIXONE()
#endif

// Real font tables straight from the library the board builds against
// (-I .pio/libdeps/cyd/TFT_eSPI).
#include <Fonts/Font16.c>
#include <Fonts/Font32rle.c>
#include <Fonts/glcdfont.c>

struct TxtBox {
  int x, y, w, h;
  char s[64];
};

class TFT_eSPI {
 public:
  uint16_t fb[320 * 240];
  long oob = 0;
  int oobX = 0, oobY = 0;
  std::vector<TxtBox> boxes;  // ink boxes recorded since the last boxes.clear()
  bool track = true;

  TFT_eSPI() { memset(fb, 0, sizeof fb); }

  // ---- API used by DisplayUI ----
  void init() {}
  void setRotation(uint8_t) {}

  void setTextColor(uint16_t f) { fgc = bgc = f; }
  void setTextColor(uint16_t f, uint16_t b) { fgc = f; bgc = b; }
  void setTextFont(uint8_t f) { fontNo = f; }
  void loadFont(const uint8_t *data) { sfont = data; loadMetrics(); }
  void unloadFont() { sfont = nullptr; }

  int16_t fontHeight() const {
    if (sfont) return (int16_t)(mMaxAscent + mMaxDescent);  // = gFont.yAdvance
    if (fontNo == 2) return 16;
    if (fontNo >= 3) return 26;
    return 8;
  }

  int32_t textWidth(const char *s) const { return textWidth(s, fontNo); }
  int32_t textWidth(const char *s, uint8_t f) const {
    if (!s) return 0;
    if (sfont) {
      // Exact replica of TFT_eSPI::textWidth smooth path: leading negative
      // gdX is folded into the width, and the LAST glyph contributes its ink
      // extent (gdX+gWidth) instead of the advance.
      int32_t strw = 0;
      for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == ' ') { strw += mSpaceW; continue; }
        uint32_t h, w, adv;
        int32_t dY, dX;
        if (!findGlyph(*p, h, w, adv, dY, dX)) { strw += mSpaceW + 1; continue; }
        if (strw == 0 && dX < 0) strw -= dX;
        strw += p[1] ? (int32_t)adv : (dX + (int32_t)w);
      }
      return strw;
    }
    if (f == 2) return bitmapWidth(s, 0);
    if (f >= 3) return bitmapWidth(s, 1);
    return 6 * (int32_t)strlen(s);  // GLCD advance
  }

  // Top-left origin (TFT_eSPI default datum when no datum is set).
  int32_t drawString(const char *s, int32_t x, int32_t y) { return drawString(s, x, y, fontNo); }
  int32_t drawString(const char *s, int32_t x, int32_t y, uint8_t f) {
    if (!s || !*s) return 0;
    Ink ink;
    if (sfont) renderSmooth(s, x, y, ink);
    else if (f == 2) renderBitmap(s, x, y, ink, 0);
    else if (f >= 3) renderBitmap(s, x, y, ink, 1);
    else renderGlcd(s, x, y, ink);
    if (track && ink.any) {
      TxtBox b;
      b.x = ink.x0; b.y = ink.y0; b.w = ink.x1 - ink.x0 + 1; b.h = ink.y1 - ink.y0 + 1;
      strncpy(b.s, s, sizeof b.s - 1); b.s[sizeof b.s - 1] = 0;
      boxes.push_back(b);
    }
    return 0;
  }

  // ---- primitives (clip + count out-of-bounds writes like the original mock) ----
  void fillScreen(uint16_t c) { for (uint32_t i = 0; i < 320u * 240u; i++) fb[i] = c; }
  void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) {
    for (int32_t j = 0; j < h; j++) px(x, y + j, w, c);
  }
  void drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) {
    drawFastHLine(x, y, w, c); drawFastHLine(x, y + h - 1, w, c);
    drawFastVLine(x, y, h, c); drawFastVLine(x + w - 1, y, h, c);
  }
  void drawFastHLine(int32_t x, int32_t y, int32_t w, uint16_t c) { px(x, y, w, c); }
  void drawFastVLine(int32_t x, int32_t y, int32_t h, uint16_t c) {
    for (int32_t j = 0; j < h; j++) one(x, y + j, c);
  }
  void drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c) {  // Bresenham
    int32_t dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int32_t sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
      one(x0, y0, c);
      if (x0 == x1 && y0 == y1) break;
      int32_t e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  }
  void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c) {
    for (int32_t j = 0; j < h; j++) {
      int32_t ins = roundInset(j, h, r);
      px(x + ins, y + j, w - 2 * ins, c);
    }
  }
  void drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c) {
    for (int32_t j = 0; j < h; j++) {
      int32_t ins = roundInset(j, h, r);
      if (j == 0 || j == h - 1) { px(x, y + j, w, c); continue; }
      one(x + ins, y + j, c);
      one(x + w - 1 - ins, y + j, c);
    }
  }
  void fillCircle(int32_t cx, int32_t cy, int32_t r, uint16_t c) {
    for (int32_t j = -r; j <= r; j++) {
      int32_t d = (int32_t)sqrtf((float)(r * r - j * j) + 0.5f);
      px(cx - d, cy + j, 2 * d + 1, c);
    }
  }
  void drawCircle(int32_t cx, int32_t cy, int32_t r, uint16_t c) {
    for (int32_t a = 0; a < 360; a += 2) {
      one(cx + (int32_t)(r * cosf(a * 3.14159265f / 180.0f)),
          cy + (int32_t)(r * sinf(a * 3.14159265f / 180.0f)), c);
    }
  }

 private:
  uint16_t fgc = 0xFFFF, bgc = 0xFFFF;
  uint8_t fontNo = 1;
  const uint8_t *sfont = nullptr;

  struct Ink {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    bool any = false;
    void add(int x, int y) {
      if (!any) { x0 = x1 = x; y0 = y1 = y; any = true; return; }
      if (x < x0) x0 = x; if (x > x1) x1 = x;
      if (y < y0) y0 = y; if (y > y1) y1 = y;
    }
  };

  static int32_t roundInset(int32_t j, int32_t h, int32_t r) {
    if (r <= 0) return 0;
    int32_t band = j < h - 1 - j ? j : h - 1 - j;
    if (band >= r) return 0;
    int32_t d = r - band;  // vertical distance from the corner centre
    return r - (int32_t)sqrtf((float)(r * r - d * d) + 0.5f);
  }

  void one(int32_t x, int32_t y, uint16_t c) {
    PIXONE();
    if (x < 0 || y < 0 || x >= 320 || y >= 240) {
      if (!oob) { oobX = (int)x; oobY = (int)y; }
      oob++;
      return;
    }
    fb[y * 320 + x] = c;
  }
  void px(int32_t x, int32_t y, int32_t w, uint16_t c) {  // clipped run
    PIXCHK();
    if (w <= 0) return;
    if (y < 0 || y >= 240) { oob++; if (!oobX && !oobY) { oobX = (int)x; oobY = (int)y; } return; }
    if (x < 0) { oob += -x; w += x; x = 0; if (w <= 0) return; }
    if (x + w > 320) { oob += x + w - 320; w = 320 - x; }
    uint16_t *row = fb + y * 320;
    for (int32_t i = 0; i < w; i++) row[x + i] = c;
  }

  // ---- SmoothFont (.vlw) rendering: header + 28-byte glyph entries, then
  // concatenated 8-bit alpha bitmaps. Same layout as TFT_eSPI's loader; the
  // tables come from include/SmoothFonts.h on both device and host. ----
  uint32_t vlwCount() const { return be32(sfont, 0); }
  uint32_t vlw(int which) const { return be32(sfont, 8 + 4 * which); }  // pts=1 ascent=2 descent=3
  uint16_t mMaxAscent = 0, mMaxDescent = 0, mSpaceW = 0;
  void loadMetrics() {  // mirrors loadFont(const uint8_t*) + the loader loop
    uint32_t n = vlwCount();
    mSpaceW = (uint16_t)((vlw(2) + vlw(3)) * 2 / 7);
    mMaxAscent = 0; mMaxDescent = 0;
    for (uint32_t i = 0; i < n; i++) {
      uint32_t u, h, w, adv;
      int32_t dY, dX;
      glyphFields((int)i, u, h, w, adv, dY, dX);
      if ((uint32_t)dY > mMaxAscent) mMaxAscent = (uint16_t)dY;
      uint32_t desc = h - (uint32_t)dY;
      if (desc > mMaxDescent) mMaxDescent = (uint16_t)desc;
    }
  }
  static uint32_t be32(const uint8_t *p, size_t o) {
    return ((uint32_t)p[o] << 24) | ((uint32_t)p[o + 1] << 16) | ((uint32_t)p[o + 2] << 8) | p[o + 3];
  }
  void glyphFields(int i, uint32_t &uni, uint32_t &h, uint32_t &w, uint32_t &adv,
                   int32_t &dY, int32_t &dX) const {
    const uint8_t *e = sfont + 24 + 28 * (size_t)i;
    uni = be32(e, 0); h = be32(e, 4); w = be32(e, 8); adv = be32(e, 12); dY = (int32_t)be32(e, 16);
    dX = (int32_t)be32(e, 20);
  }
  const uint8_t *findGlyph(uint32_t uni, uint32_t &h, uint32_t &w, uint32_t &adv, int32_t &dY,
                           int32_t &dX) const {
    uint32_t n = vlwCount();
    size_t off = 24 + 28 * n;
    for (uint32_t i = 0; i < n; i++) {
      uint32_t u, hh, ww, aa;
      int32_t yy, xx;
      glyphFields((int)i, u, hh, ww, aa, yy, xx);
      if (u == uni) { h = hh; w = ww; adv = aa; dY = yy; dX = xx; return sfont + off; }
      off += (size_t)hh * ww;
    }
    return nullptr;
  }
  void renderSmooth(const char *s, int32_t x, int32_t y, Ink &ink) {
    uint32_t xadv = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
      uint32_t h, w, adv;
      int32_t dY, dX;
      const uint8_t *bmp = findGlyph(*p, h, w, adv, dY, dX);
      if (bmp) {
        // TFT_eSPI drawGlyph: cy = cursor_y + maxAscent - gdY (y is the cell top).
        int32_t gx = x + (int32_t)xadv + dX, gy = y + (int32_t)mMaxAscent - dY;
        for (uint32_t r = 0; r < h; r++)
          for (uint32_t c = 0; c < w; c++) {
            uint8_t a = bmp[r * w + c];
            if (!a) continue;  // card bg is painted by the caller's fillRect
            int32_t X = gx + (int32_t)c, Y = gy + (int32_t)r;
            one(X, Y, a == 255 ? fgc : peekBlend(X, Y, a));
            ink.add((int)X, (int)Y);
          }
      }
      xadv += adv;
    }
  }
  uint16_t peekBlend(int32_t X, int32_t Y, uint8_t a) {  // AA over the current pixel
    if (X < 0 || Y < 0 || X >= 320 || Y >= 240) { oob++; if (!oobX && !oobY) { oobX = (int)X; oobY = (int)Y; } return fgc; }
    uint16_t cur = fb[Y * 320 + X];
    uint32_t r = (((fgc >> 11) & 31) * a + ((cur >> 11) & 31) * (255 - a)) / 255;
    uint32_t g = (((fgc >> 5) & 63) * a + ((cur >> 5) & 63) * (255 - a)) / 255;
    uint32_t b = ((fgc & 31) * a + (cur & 31) * (255 - a)) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
  }

  // ---- Built-in bitmap fonts: 0 => Font16 (f16), 1 => Font32 RLE (f32rle). ----
  int32_t bitmapWidth(const char *s, int which) const {
    int32_t w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
      w += which == 0 ? widtbl_f16[*p - 32] : widtbl_f32[*p - 32];
    return w;
  }
  void renderBitmap(const char *s, int32_t x, int32_t y, Ink &ink, int which) {
    int32_t cx = x;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
      int c = *p - 32;
      if (c < 0 || c >= 96) continue;
      if (which == 0) {
        const uint8_t *g = chrtbl_f16[c];
        int32_t w = widtbl_f16[c];
        int wbytes = (w + 6) / 8;
        for (int row = 0; row < 16; row++)
          for (int k = 0; k < wbytes; k++) {
            uint8_t line = pgm_read_byte(g + row * wbytes + k);
            for (int b = 0; b < 8; b++)
              if ((k * 8 + b < w) && (line & (0x80 >> b))) { one(cx + k * 8 + b, y + row, fgc); ink.add((int)(cx + k * 8 + b), (int)(y + row)); }
          }
        cx += w;
      } else {
        const uint8_t *g = chrtbl_f32[c];
        int32_t w = widtbl_f32[c];
        int32_t pc = 0, total = w * 26;
        size_t bi = 0;
        while (pc < total) {
          uint8_t v = pgm_read_byte(g + bi++);
          int32_t n;
          if (v & 0x80) {
            n = (v & 0x7F) + 1;
            for (int32_t k = 0; k < n; k++) {
              int32_t X = cx + pc % w, Y = y + pc / w;
              one(X, Y, fgc); ink.add((int)X, (int)Y);
              pc++;
            }
          } else {
            n = (int32_t)v + 1;  // device: line++ also on blank runs
            pc += n;
          }
        }
        cx += w;
      }
    }
  }
  void renderGlcd(const char *s, int32_t x, int32_t y, Ink &ink) {
    int32_t cx = x;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
      for (int col = 0; col < 5; col++) {
        uint8_t bits = pgm_read_byte(&font[(size_t)*p * 5 + col]);
        for (int row = 0; row < 7; row++)
          if (bits & (1 << row)) { one(cx + col, y + row, fgc); ink.add((int)(cx + col), (int)(y + row)); }
      }
      cx += 6;
    }
  }
};
