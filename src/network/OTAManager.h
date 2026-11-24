// OTAManager.h - firmware update over Wi-Fi.
//   * Web:  POST /update (multipart "firmware"), streamed straight into the Updater.
//   * espota / Arduino IDE: ArduinoOTA on port 3232, password = admin password when one is set.
// An update is refused while a flash write is in progress; Yahoo requests are aborted
// first so the TLS heap is free. Restart happens only after the response was sent.
#pragma once
#include <Arduino.h>
#include <WebServer.h>

namespace ota {
void begin(const char *hostname, const char *password);   // ArduinoOTA
void loop();
bool active();                            // an update is running: the rest of the app must stay out of the way
// Web upload handlers (register with server.on("/update", HTTP_POST, ota::webDone, ota::webUpload)).
void webUpload(WebServer &server, bool authorized);
void webDone(WebServer &server, bool authorized);
const char *lastError();
bool restartPending();
void scheduleRestart(uint32_t delayMs);   // restart once storage is idle and the delay elapsed
void restartLoop();
}
