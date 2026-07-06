// =============================================================================
// qr_code.cpp  —  see qr_code.h
// =============================================================================
#include "qr_code.h"

#include <qrcode.h>  // ricmoo/QRCode

#include "photobooth_config.h"

void qrBuildUrl(const char* sessionId, char* out, size_t cap) {
  snprintf(out, cap, "http://%s/s/%s", AP_IP, sessionId);
}

void qrRenderToCanvas(lv_obj_t* canvas, const char* text, int sizePx) {
  // Version 3 (29x29 modules) at ECC LOW comfortably holds a short LAN URL.
  QRCode qr;
  const int version = 3;
  uint8_t qrData[qrcode_getBufferSize(version)];
  qrcode_initText(&qr, qrData, version, ECC_LOW, text);

  const int modules = qr.size;                 // e.g. 29
  const int scale = sizePx / modules;          // integer pixels per module
  const int drawn = scale * modules;           // actual pixel span used
  const int offset = (sizePx - drawn) / 2;     // center within the canvas

  lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);
  lv_color_t black = lv_color_black();
  for (int y = 0; y < modules; ++y) {
    for (int x = 0; x < modules; ++x) {
      if (!qrcode_getModule(&qr, x, y)) continue;
      // Fill this module as a scale x scale block of black pixels.
      for (int dy = 0; dy < scale; ++dy) {
        for (int dx = 0; dx < scale; ++dx) {
          lv_canvas_set_px(canvas, offset + x * scale + dx,
                           offset + y * scale + dy, black);
        }
      }
    }
  }
}
