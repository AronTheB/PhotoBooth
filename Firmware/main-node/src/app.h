// =============================================================================
// app.h  —  Shared types and display geometry for the main node.
// =============================================================================
#pragma once

#include <Arduino.h>

#include "photobooth_protocol.h"

// ---- Display geometry -------------------------------------------------------
// !! CONFIRM against the exact VIEWE 4.3" model. 800x480 is the common size for
// these panels; the RGB timing/pins live in display_bsp.cpp. !!
#define DISP_WIDTH 800
#define DISP_HEIGHT 480

// Native preview frame size decoded from the camera's QVGA JPEG. The preview
// image is decoded at this size and zoomed to fill the screen by LVGL.
#define PREVIEW_W 320
#define PREVIEW_H 240

// A 4-character session id like "A1B2".
static const int SESSION_ID_LEN = 4;

// Preview streams several frames/sec, so this much silence on the UART link
// means the camera node is missing or disconnected.
static const uint32_t NO_CAMERA_TIMEOUT_MS = 3000;
