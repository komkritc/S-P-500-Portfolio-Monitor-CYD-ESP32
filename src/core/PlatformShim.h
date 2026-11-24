// ESP32 replacements for ESP8266-only ESP helpers used by the ported
// SmallTV modules. Include from the .cpp that uses them.
#pragma once
#include <Arduino.h>
#include <esp_heap_caps.h>

// ESP8266's ESP.getMaxFreeBlockSize(): largest contiguous DRAM block.
static inline uint32_t maxFreeBlock() { return heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT); }

// ESP8266's ESP.getHeapFragmentation(): percent of free heap NOT in the
// largest block.
static inline uint32_t heapFragPct() {
  const uint32_t f = ESP.getFreeHeap(), b = maxFreeBlock();
  return f ? (uint32_t)(100u - (b * 100u) / f) : 0u;
}

// ESP8266's ESP.getChipId(): low 24 bits of the eFuse MAC.
static inline uint32_t chipIdLow() { return (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFu); }
