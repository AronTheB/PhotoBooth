// =============================================================================
// display_bsp.h  —  Board support: LVGL <-> VIEWE 4.3" RGB panel + touch.
//
// This is the ONE board-specific file. Everything else in main-node is generic.
// The RGB panel timing, the panel data pins, and the touch controller pins MUST
// be copied from the VIEWE demo for your exact model — the values in the .cpp
// are typical-but-unverified placeholders (build plan §2 open item).
// =============================================================================
#pragma once

#include <lvgl.h>

// Initialize the panel, register the LVGL display + touch input device, and set
// up draw buffers in PSRAM. Call once, before building any UI. Returns false if
// the panel/LVGL could not be initialized.
bool displayBspBegin();

// Push the backlight to a level 0..255 (used for the capture flash boost).
void displaySetBacklight(uint8_t level);

// Bring-up diagnostics: total touch reports seen since boot and the last point.
// Lets the UI show on-screen whether the GT911 is delivering anything at all
// (USB serial is unreliable on this board, so the screen is the debug console).
uint32_t displayTouchCount();
void displayLastTouch(int16_t* x, int16_t* y);
