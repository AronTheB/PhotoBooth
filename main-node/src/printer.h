// =============================================================================
// printer.h  —  Xprinter P203A thermal printing over BLE.
//
// A print job runs on its own task (core 0): for each photo of the session it
// loads the JPEG from SD, decodes at half scale, crops to the same band the
// booth screen showed (WYSIWYG, like the web gallery), rotates 90° so the
// print uses the full 384-dot head width, Floyd-Steinberg dithers to 1-bit,
// and streams it to the printer as ESC/POS raster over BLE.
//
// The UI polls printerStatus()/printerBusy(); all LVGL updates stay on the
// UI core.
// =============================================================================
#pragma once

#include <Arduino.h>

// Init the BLE stack. Call once from setup(), after WiFi is up.
void printerBegin();

// True while a job (connect/decode/print) is running.
bool printerBusy();

// Short status line for the UI ("" when idle). Latched result messages
// ("Print done" / "Printer not found") clear themselves after a few seconds.
const char* printerStatus();

// Print photos 1..photoCount of the session. Returns false if a job is
// already running. Asynchronous — poll printerBusy()/printerStatus().
bool printerStartJob(const char* sessionId, int photoCount);
