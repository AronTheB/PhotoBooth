// =============================================================================
// uart_link.h  —  UART receiver task + command sending (build plan §6a).
//
// The receiver runs on its own FreeRTOS task pinned to core 0, so it never
// competes with LVGL/UI on core 1. It parses frames and stashes the latest
// preview JPEG and the most recent capture JPEG into PSRAM buffers guarded by
// mutexes. The UI core pulls from those buffers (decode is done on the UI core
// because LVGL is single-threaded).
// =============================================================================
#pragma once

#include <Arduino.h>

// Start the UART, allocate buffers, and spawn the receiver task on core 0.
void uartLinkBegin();

// ---- Preview frames ---------------------------------------------------------
// Copy the latest preview JPEG (if a new one has arrived since the last call)
// into `dst` (capacity `cap`). Returns the byte length copied, or 0 if there is
// no new frame. Thread-safe.
size_t uartTakeLatestPreview(uint8_t* dst, size_t cap);

// ---- Capture (full-res still) ----------------------------------------------
// Non-blocking check: has a CAPTURE_FRAME arrived since the last consume?
bool uartCaptureReady();

// Consume the pending capture JPEG into `dst`. Returns byte length, or 0 if
// none pending / too big for `cap`. Clears the ready flag.
size_t uartConsumeCapture(uint8_t* dst, size_t cap);

// Clear any stale pending capture (call before issuing a fresh CMD_CAPTURE).
void uartClearCapture();

// ---- Commands to the camera node -------------------------------------------
void uartSendStartPreview();
void uartSendStopPreview();
void uartSendCapture();

// ---- Link liveness ----------------------------------------------------------
// Milliseconds since the last valid frame arrived from the camera node. Returns
// a large sentinel (>= any sensible timeout) if no frame has EVER been
// received, so callers can treat "never seen" and "gone quiet" the same way.
uint32_t uartMsSinceLastFrame();

// ---- Diagnostics (milestone 1) ---------------------------------------------
uint32_t uartFramesOk();
uint32_t uartFramesBadCrc();
// Raw bytes read off the wire, valid or not. Distinguishes "nothing arrives
// electrically" (0) from "bytes arrive but don't parse" (climbing, ok=0).
uint32_t uartBytesRx();
