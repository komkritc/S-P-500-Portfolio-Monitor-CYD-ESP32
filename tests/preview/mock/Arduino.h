// Host mock of the Arduino core for the CYD preview harness (ESP32 flavour).
// Just enough for core/finance modules + DisplayUI.cpp to compile and run on
// a PC. millis() is driven by the harness so update()'s 4 Hz gate is testable.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <algorithm>
using std::min;
using std::max;

#define PROGMEM
#define PSTR(x) x
#define pgm_read_byte(p) (*(const uint8_t *)(p))
#define pgm_read_word(p) (*(const uint16_t *)(p))
#define pgm_read_dword(p) (*(const uint32_t *)(p))
#define memcpy_P memcpy
#define F(x) (x)

extern uint32_t g_arduinoMs;
inline uint32_t millis() { return g_arduinoMs; }
inline uint32_t micros() { return g_arduinoMs * 1000u; }
inline void delay(uint32_t ms) { g_arduinoMs += ms; }
inline void delay(unsigned long ms) { g_arduinoMs += ms; }

template <typename T> inline T constrain(T v, T lo, T hi) { return v < lo ? lo : v > hi ? hi : v; }
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// Serial sink: everything goes to a FILE so device logs become greppable in the
// harness output without touching stdout's report lines.
struct SerialMock {
  void printf(const char *fmt, ...);
  void print(const char *s);
  void print(int v);
  void println(const char *s = "");
  void println(int v);
  void println(long v);
  void begin(unsigned long) {}
};
extern SerialMock Serial;
