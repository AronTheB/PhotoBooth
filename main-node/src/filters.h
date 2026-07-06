// =============================================================================
// filters.h  —  Decorative preview overlays ("filters") loaded from the SD.
//
// A filter is a transparent PNG on the SD card in /filters (e.g. hearts around
// the screen edge). Adding one needs NO reflash: drop an 800x480 32-bit PNG
// (with alpha) into /filters and it appears in the arrow rotation on the idle
// screen. Smaller PNGs work too — they are centered.
//
// Only the active filter is kept decoded (one shared PSRAM buffer), so the
// number of PNGs on the card is effectively unlimited; switching costs a brief
// SD read + decode.
// =============================================================================
#pragma once

#include <lvgl.h>

// Scan /filters on the (already mounted) SD card. Safe to call when the SD
// failed to mount — you just end up with zero filters.
void filtersBegin();

// How many overlay PNGs were found.
int filtersCount();

// Display name of filter `idx` (0-based, filename without ".png").
const char* filtersName(int idx);

// Bare filename with extension (e.g. "hearts.png") — used to tag saved shots
// and to build the /f/<file> URL the web gallery composites from.
const char* filtersFileName(int idx);

// Human-readable reason the last filtersLoad() failed (e.g. "hearts: 900x600
// too big (max 800x480)"). Shown in the on-screen banner.
const char* filtersLastError();

// Decode filter `idx` into the shared overlay buffer and return an image
// descriptor for it (full-screen, transparent where the PNG is transparent).
// Returns nullptr on decode failure. The returned pointer is always the same
// descriptor — invalidate LVGL's image cache after each call.
const lv_img_dsc_t* filtersLoad(int idx);
