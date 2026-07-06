// =============================================================================
// main-node / main.cpp  —  VIEWE 4.3" board firmware (build plan §6)
//
// Wires the modules together and runs the two concurrent concerns:
//   * core 0: UART receiver task (spawned inside uartLinkBegin) — never starves
//     the UI, per build plan §6/§11.
//   * core 1 (this loop): LVGL, preview decode/render, session state machine,
//     and the HTTP server.
// =============================================================================
#include <Arduino.h>
#include <lvgl.h>

#include "cloud.h"
#include "display_bsp.h"
#include "filters.h"
#include "printer.h"
#include "session.h"
#include "storage.h"
#include "uart_link.h"
#include "ui.h"
#include "web_server.h"

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[main] VIEWE ESP32-S3 photo booth main node");

  if (!psramFound()) {
    Serial.println("[main] WARNING: PSRAM not found — this will not work");
  }

  // Display + LVGL first so we can show status if anything else fails.
  if (!displayBspBegin()) {
    Serial.println("[main] display init failed — halting");
    while (true) delay(1000);
  }
  uiBegin();

  // SD is required for saving photos and serving them.
  if (!storageBegin()) {
    uiShowMessage("No SD card");
  } else {
    filtersBegin();      // overlay PNGs live on the SD in /filters
    uiFiltersRefresh();  // reveal the filter arrows if any were found
  }

  uartLinkBegin();   // spawns the receiver task on core 0
  webServerBegin();  // softAP + HTTP (+ STA uplink when configured)
  cloudBegin();      // background session uploader (needs the STA uplink)
  printerBegin();    // BLE client for the thermal printer (after WiFi is up)
  sessionBegin();

  Serial.println("[main] ready");
}

void loop() {
  lv_timer_handler();  // LVGL: process input, redraw
  uiRenderPreview();   // decode + show the freshest preview frame
  sessionLoop();       // advance the session state machine
  webServerLoop();     // service any pending HTTP client

  delay(2);  // keep the loop tight but yield to the idle/WiFi tasks
}
