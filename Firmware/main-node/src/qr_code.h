// =============================================================================
// qr_code.h  —  QR bitmap generation for the result screen (build plan §6e).
//
// Wraps the ricmoo/QRCode library to produce an LVGL canvas showing the URL
// http://<AP_IP>/s/<sessionId>.
// =============================================================================
#pragma once

#include <Arduino.h>
#include <lvgl.h>

// Build the gallery URL for a session into `out` (>= 40 bytes).
void qrBuildUrl(const char* sessionId, char* out, size_t cap);

// Render a QR code for `text` into the given LVGL canvas object. The canvas is
// (re)sized to a square of `sizePx` and filled with black/white modules.
// The canvas must already have a backing buffer of at least
// sizePx*sizePx*sizeof(lv_color_t) bytes (see ui.cpp).
void qrRenderToCanvas(lv_obj_t* canvas, const char* text, int sizePx);
