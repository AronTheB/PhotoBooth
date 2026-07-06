// =============================================================================
// ui.h  —  LVGL screens + live preview rendering (build plan §6b/§6c).
//
// The session state machine (session.cpp) drives the UI through these calls.
// All of these MUST be called from the UI task/core (LVGL is single-threaded).
// =============================================================================
#pragma once

#include <Arduino.h>

void uiBegin();  // build all screens; show idle

// Pull the freshest preview JPEG from the UART link, decode it, and update the
// on-screen preview image. Call every UI loop iteration.
void uiRenderPreview();

// Show/hide the filter arrows based on filtersCount(). Call once after
// filtersBegin() (the SD mounts after the UI is built).
void uiFiltersRefresh();

// --- Idle screen inputs ---
int uiSelectedShotCount();       // currently selected 1 / 2 / 4
bool uiPrintRequested();         // true once after result "Print" (clears)

// Show a printing status line on the result screen ("" restores the normal
// WiFi hint). Call from the UI core only.
void uiSetPrintStatus(const char* s);

// PNG filename of the active filter ("" when none) — recorded per shot so the
// web gallery can burn the overlay into the downloaded image.
const char* uiActiveFilterFile();
bool uiTakePhotoRequested();     // true once after the button is pressed (clears)
bool uiDoneRequested();          // true once after result "Done" (clears)

// --- Screen transitions ---
void uiShowIdle();
void uiShowResult(const char* sessionId, int photoCount);

// Pause/resume the live preview. While frozen, uiRenderPreview keeps the last
// decoded frame on screen — used during capture so the guest sees a "held"
// shot instead of the sensor's mode-switch glitches.
void uiFreezePreview(bool freeze);

// --- Overlays driven during a session ---
void uiShowCountdown(int n);  // large centered number
void uiHideCountdown();
void uiSetFlash(bool on);     // full-screen white flash
void uiShowMessage(const char* msg);  // transient status/error banner
