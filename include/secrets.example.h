// Copy to include/secrets.h (git-ignored) and fill in.
// The firmware boots and serves the LCD with blank credentials; it just
// stays OFFLINE until they are set.
#pragma once

#define WIFI_SSID ""
#define WIFI_PASS ""

// ArduinoOTA password, "" = OTA open on the LAN.
#define OTA_PASS ""

// Device timezone offset from UTC in seconds (Bangkok = 7*3600).
#define TZ_OFFSET_SEC (7 * 3600)
