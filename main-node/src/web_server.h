// =============================================================================
// web_server.h  —  WiFi softAP + HTTP gallery server (build plan §6e / §8).
//
// Brings up an open softAP (SSID "PhotoBooth", IP 192.168.4.1) and serves:
//   GET /s/<id>          -> a small HTML gallery page for that session
//   GET /s/<id>/<n>.jpg  -> the raw photo from SD
//
// Doing the "frame/layout" in served HTML/CSS keeps all image compositing off
// the ESP32 (build plan §9). Call webServerLoop() from the main loop.
// =============================================================================
#pragma once

#include <Arduino.h>

void webServerBegin();  // start AP + HTTP server
void webServerLoop();   // service pending HTTP clients (call from loop())

// Take the softAP down / bring it back. The BLE printer connection loses the
// radio-share fight against AP beacons, so the print job pauses WiFi for its
// duration; phones re-join automatically when the AP returns.
void webServerPauseAP();
void webServerResumeAP();

// Captive portal target: the session whose gallery a freshly-joined phone is
// redirected to. Set when a session's result screen is shown.
void webServerSetActiveSession(const char* id);
