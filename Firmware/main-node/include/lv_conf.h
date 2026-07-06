// =============================================================================
// lv_conf.h  —  LVGL 8.3 configuration for the photo booth main node.
//
// Only options that differ from LVGL's built-in defaults (lv_conf_internal.h)
// are set here; everything else falls back to those defaults. Enabled via the
// build flag -DLV_CONF_INCLUDE_SIMPLE.
//
// NOTE on colors: Arduino_GFX draw16bitRGBBitmap expects native-endian RGB565.
// If the preview looks color-swapped (reds/blues inverted), set
// LV_COLOR_16_SWAP to 1 here (or switch flushCb to draw16bitBeRGBBitmap).
// =============================================================================
#if 1  // set to 1 to enable this config

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

// ---- Color ----
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

// ---- Memory (LVGL's own heap for objects) ----
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (64U * 1024U)

// ---- Tick: we call lv_tick_inc() ourselves from a task (display_bsp.cpp) ----
#define LV_TICK_CUSTOM 0

// ---- Fonts used by the UI ----
#define LV_FONT_MONTSERRAT_14 1  // default (diag bar, small text)
#define LV_FONT_MONTSERRAT_20 1  // buttons, banners, info text
#define LV_FONT_MONTSERRAT_28 1  // titles, shutter icon
#define LV_FONT_MONTSERRAT_48 1  // countdown number
#define LV_FONT_DEFAULT &lv_font_montserrat_14

// ---- Keep the image cache small; we invalidate the preview every frame ----
#define LV_IMG_CACHE_DEF_SIZE 1

#endif  // LV_CONF_H
#endif  // enable guard
