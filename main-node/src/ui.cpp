// =============================================================================
// ui.cpp  —  see ui.h
// =============================================================================
#include "ui.h"

#include <JPEGDEC.h>
#include <lvgl.h>

#include "app.h"
#include "cloud.h"
#include "display_bsp.h"
#include "filters.h"
#include "qr_code.h"
#include "uart_link.h"

// ---------------------------------------------------------------------------
// Palette — one accent color used everywhere so the UI reads as designed.
// ---------------------------------------------------------------------------
#define COL_ACCENT lv_color_hex(0xFF4D6D)    // warm coral: shutter ring, pills
#define COL_BG_DARK lv_color_hex(0x101016)   // result screen background
#define COL_CARD lv_color_hex(0x1C1C24)      // overlay cards
#define COL_TEXT_DIM lv_color_hex(0x9AA0A6)  // secondary text

// ---------------------------------------------------------------------------
// Screens & widgets
// ---------------------------------------------------------------------------
static lv_obj_t* g_idle_scr = nullptr;
static lv_obj_t* g_result_scr = nullptr;

static lv_obj_t* g_preview_img = nullptr;   // live preview (on idle screen)
static lv_obj_t* g_overlay_deco = nullptr;  // §9 decoration slot (unused v1)
static lv_obj_t* g_count_circle = nullptr;  // countdown badge (circle + number)
static lv_obj_t* g_countdown_lbl = nullptr;
static lv_obj_t* g_flash = nullptr;
static lv_obj_t* g_msg = nullptr;
static lv_obj_t* g_nocam = nullptr;  // "No camera" overlay (idle screen)
static lv_obj_t* g_diag = nullptr;   // bring-up: link + touch stats overlay

static lv_obj_t* g_qr_canvas = nullptr;
static lv_obj_t* g_result_info = nullptr;
static lv_obj_t* g_result_instr = nullptr;   // WiFi hint line
static lv_obj_t* g_print_status = nullptr;   // printing progress line

// QR code edge length. Small enough that the info text below the card clears
// the Done button on the 480px-tall panel.
static const int kQrSize = 200;

// Shot-count selector state.
static const int kShotCounts[] = SHOT_COUNTS;
static const int kNumShotCounts = sizeof(kShotCounts) / sizeof(kShotCounts[0]);
static int g_selected_idx = 0;  // index into kShotCounts
static lv_obj_t* g_shot_btns[8];

// One-shot input flags (consumed by the state machine).
static volatile bool g_take_requested = false;
static volatile bool g_done_requested = false;
static volatile bool g_print_requested = false;

// Filter rotation state. Index 0 = no filter, 1..N = filtersLoad(idx-1).
static int g_filter_idx = 0;
static lv_obj_t* g_arrow_l = nullptr;
static lv_obj_t* g_arrow_r = nullptr;

// Transient banner auto-hide deadline (0 = not scheduled). Only banners shown
// via showTransient() self-hide; uiShowMessage() banners stay until cleared.
static uint32_t g_msg_hide_at = 0;

// While true, uiRenderPreview holds the last decoded frame on screen.
static volatile bool g_preview_frozen = false;

// ---------------------------------------------------------------------------
// Preview: decode QVGA JPEG -> RGB565 buffer, shown via an lv_img zoomed to fill
// ---------------------------------------------------------------------------
static JPEGDEC g_jpeg;
static lv_color_t* g_preview_px = nullptr;  // PREVIEW_W * PREVIEW_H, PSRAM
static lv_img_dsc_t g_preview_dsc;
static uint8_t* g_preview_jpeg = nullptr;   // scratch for the incoming JPEG

// JPEGDEC calls this for each decoded block; copy it into the full-frame buffer.
static int jpegDrawCb(JPEGDRAW* p) {
  for (int y = 0; y < p->iHeight; ++y) {
    int dy = p->y + y;
    if (dy < 0 || dy >= PREVIEW_H) continue;
    lv_color_t* dst = &g_preview_px[dy * PREVIEW_W + p->x];
    uint16_t* src = &p->pPixels[y * p->iWidth];
    int w = p->iWidth;
    if (p->x + w > PREVIEW_W) w = PREVIEW_W - p->x;
    memcpy(dst, src, w * sizeof(uint16_t));
  }
  return 1;
}

// Show/hide the "No camera" overlay based on how long the UART link has been
// silent. Only meaningful on the idle screen, where preview should be live.
static void updateCameraStatus() {
  if (lv_scr_act() != g_idle_scr) return;

  // Refresh the on-screen diagnostic bar once a second.
  static uint32_t last_diag_ms = 0;
  uint32_t now = millis();
  if (now - last_diag_ms >= 1000) {
    last_diag_ms = now;
    int16_t tx, ty;
    displayLastTouch(&tx, &ty);
    lv_label_set_text_fmt(g_diag,
                          "rx=%u ok=%u bad=%u  |  touch n=%u last=%d,%d",
                          (unsigned)uartBytesRx(), (unsigned)uartFramesOk(),
                          (unsigned)uartFramesBadCrc(),
                          (unsigned)displayTouchCount(), tx, ty);
  }

  bool missing = uartMsSinceLastFrame() > NO_CAMERA_TIMEOUT_MS;
  if (missing == lv_obj_has_flag(g_nocam, LV_OBJ_FLAG_HIDDEN)) {
    // State changed: reveal when missing, hide when a frame has arrived.
    if (missing)
      lv_obj_clear_flag(g_nocam, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(g_nocam, LV_OBJ_FLAG_HIDDEN);
  }
}

void uiRenderPreview() {
  updateCameraStatus();

  // Auto-hide transient banners (filter names etc.).
  if (g_msg_hide_at && millis() > g_msg_hide_at) {
    g_msg_hide_at = 0;
    lv_obj_add_flag(g_msg, LV_OBJ_FLAG_HIDDEN);
  }

  if (g_preview_frozen) return;  // hold the current frame during capture

  size_t len = uartTakeLatestPreview(g_preview_jpeg, FRAME_MAX_PAYLOAD);
  if (len == 0) return;  // no new frame

  if (g_jpeg.openRAM(g_preview_jpeg, len, jpegDrawCb)) {
    g_jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
    g_jpeg.decode(0, 0, 0);
    g_jpeg.close();
    // Tell LVGL the image data changed and redraw.
    lv_img_cache_invalidate_src(&g_preview_dsc);
    lv_obj_invalidate(g_preview_img);
  }
}

void uiFreezePreview(bool freeze) { g_preview_frozen = freeze; }

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------
static void shotBtnCb(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  g_selected_idx = idx;
  for (int i = 0; i < kNumShotCounts; ++i) {
    if (i == idx)
      lv_obj_add_state(g_shot_btns[i], LV_STATE_CHECKED);
    else
      lv_obj_clear_state(g_shot_btns[i], LV_STATE_CHECKED);
  }
}

static void takeBtnCb(lv_event_t* e) { g_take_requested = true; }
static void doneBtnCb(lv_event_t* e) { g_done_requested = true; }
static void printBtnCb(lv_event_t* e) { g_print_requested = true; }

// Show a short-lived banner (auto-hides; used for filter names).
static void showTransient(const char* msg) {
  uiShowMessage(msg);
  g_msg_hide_at = millis() + 1500;
}

// Step the filter rotation by ±1 and swap the overlay image.
static void applyFilter(int dir) {
  int total = filtersCount() + 1;  // slot 0 is "No filter"
  if (total <= 1) return;
  g_filter_idx = (g_filter_idx + dir + total) % total;

  if (g_filter_idx == 0) {
    lv_obj_add_flag(g_overlay_deco, LV_OBJ_FLAG_HIDDEN);
    showTransient("No filter");
    return;
  }

  // Decode blocks the UI for a few hundred ms — acceptable for a tap.
  const lv_img_dsc_t* d = filtersLoad(g_filter_idx - 1);
  if (!d) {
    lv_obj_add_flag(g_overlay_deco, LV_OBJ_FLAG_HIDDEN);
    showTransient(filtersLastError());  // says WHY (size/type/decode)
    return;
  }
  // The descriptor/buffer is reused for every filter, so LVGL must drop any
  // cached decode of it before the redraw.
  lv_img_cache_invalidate_src(d);
  lv_img_set_src(g_overlay_deco, d);
  lv_obj_align(g_overlay_deco, LV_ALIGN_CENTER, 0, 0);
  lv_obj_clear_flag(g_overlay_deco, LV_OBJ_FLAG_HIDDEN);
  lv_obj_invalidate(g_overlay_deco);
  showTransient(filtersName(g_filter_idx - 1));
}

static void arrowLeftCb(lv_event_t* e) { applyFilter(-1); }
static void arrowRightCb(lv_event_t* e) { applyFilter(+1); }

// ---------------------------------------------------------------------------
// Screen construction
// ---------------------------------------------------------------------------
static void buildIdleScreen() {
  g_idle_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(g_idle_scr, lv_color_black(), 0);

  // Live preview image, zoomed to cover the whole screen.
  g_preview_img = lv_img_create(g_idle_scr);
  lv_img_set_src(g_preview_img, &g_preview_dsc);
  lv_obj_align(g_preview_img, LV_ALIGN_CENTER, 0, 0);
  // Cover 800x480 from a 320x240 source: max(800/320,480/240)=2.5 -> 640/256.
  lv_img_set_zoom(g_preview_img, 640);

  // Decoration overlay (build plan §9): the active filter PNG, drawn above
  // the preview and below all controls. Hidden until a filter is selected.
  g_overlay_deco = lv_img_create(g_idle_scr);
  lv_obj_align(g_overlay_deco, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(g_overlay_deco, LV_OBJ_FLAG_HIDDEN);

  // Filter arrows: translucent circles at mid-left / mid-right. Kept hidden
  // until uiFiltersRefresh() confirms the SD actually has filters.
  for (int side = 0; side < 2; ++side) {
    lv_obj_t* b = lv_btn_create(g_idle_scr);
    lv_obj_set_size(b, 64, 64);
    lv_obj_align(b, side ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID,
                 side ? -12 : 12, -30);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, side ? arrowRightCb : arrowLeftCb, LV_EVENT_CLICKED,
                        nullptr);
    lv_obj_t* al = lv_label_create(b);
    lv_label_set_text(al, side ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(al, lv_color_white(), 0);
    lv_obj_set_style_text_font(al, &lv_font_montserrat_28, 0);
    lv_obj_center(al);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    if (side)
      g_arrow_r = b;
    else
      g_arrow_l = b;
  }

  // --- Bottom control bar: translucent strip over the preview -------------
  lv_obj_t* bar = lv_obj_create(g_idle_scr);
  lv_obj_set_size(bar, DISP_WIDTH, 116);
  lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_40, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_set_style_pad_all(bar, 0, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

  // Shot-count pills (1 / 2 / 4) on the left: translucent circles, the
  // selected one filled with the accent color.
  for (int i = 0; i < kNumShotCounts; ++i) {
    lv_obj_t* b = lv_btn_create(bar);
    lv_obj_set_size(b, 68, 68);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, 24 + i * 84, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, COL_ACCENT, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_add_event_cb(b, shotBtnCb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text_fmt(l, "%d", kShotCounts[i]);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    g_shot_btns[i] = b;
  }
  lv_obj_add_state(g_shot_btns[0], LV_STATE_CHECKED);  // default selection

  // Small caption under nothing fancy — tells guests what the pills mean.
  lv_obj_t* pills_cap = lv_label_create(bar);
  lv_label_set_text(pills_cap, "shots");
  lv_obj_set_style_text_color(pills_cap, COL_TEXT_DIM, 0);
  lv_obj_align(pills_cap, LV_ALIGN_LEFT_MID, 24 + kNumShotCounts * 84, 0);

  // Camera-style shutter button in the middle: white circle, accent ring.
  lv_obj_t* take = lv_btn_create(bar);
  lv_obj_set_size(take, 88, 88);
  lv_obj_align(take, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(take, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(take, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(take, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(take, 5, 0);
  lv_obj_set_style_border_color(take, COL_ACCENT, 0);
  lv_obj_set_style_shadow_width(take, 20, 0);
  lv_obj_set_style_shadow_opa(take, LV_OPA_40, 0);
  lv_obj_set_style_shadow_color(take, lv_color_black(), 0);
  lv_obj_set_style_bg_color(take, lv_color_hex(0xE2E2E2), LV_STATE_PRESSED);
  lv_obj_add_event_cb(take, takeBtnCb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* ti = lv_label_create(take);
  lv_label_set_text(ti, LV_SYMBOL_IMAGE);
  lv_obj_set_style_text_color(ti, COL_ACCENT, 0);
  lv_obj_set_style_text_font(ti, &lv_font_montserrat_28, 0);
  lv_obj_center(ti);

  // Countdown badge: dark translucent circle with a big number. Hidden until
  // a session starts; uiShowCountdown pops it each second.
  g_count_circle = lv_obj_create(g_idle_scr);
  lv_obj_set_size(g_count_circle, 200, 200);
  lv_obj_center(g_count_circle);
  lv_obj_set_style_radius(g_count_circle, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(g_count_circle, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(g_count_circle, LV_OPA_50, 0);
  lv_obj_set_style_border_width(g_count_circle, 4, 0);
  lv_obj_set_style_border_color(g_count_circle, lv_color_white(), 0);
  lv_obj_set_style_border_opa(g_count_circle, LV_OPA_60, 0);
  lv_obj_clear_flag(g_count_circle, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_count_circle, LV_OBJ_FLAG_HIDDEN);

  g_countdown_lbl = lv_label_create(g_count_circle);
  lv_obj_set_style_text_font(g_countdown_lbl, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(g_countdown_lbl, lv_color_white(), 0);
  lv_label_set_text(g_countdown_lbl, "");
  lv_obj_center(g_countdown_lbl);

  // Full-screen white flash overlay (hidden by default).
  g_flash = lv_obj_create(g_idle_scr);
  lv_obj_set_size(g_flash, DISP_WIDTH, DISP_HEIGHT);
  lv_obj_set_style_bg_color(g_flash, lv_color_white(), 0);
  lv_obj_set_style_border_width(g_flash, 0, 0);
  lv_obj_add_flag(g_flash, LV_OBJ_FLAG_HIDDEN);

  // Transient message banner (errors / status): pill at the top.
  g_msg = lv_label_create(g_idle_scr);
  lv_obj_set_style_bg_color(g_msg, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(g_msg, LV_OPA_70, 0);
  lv_obj_set_style_text_color(g_msg, lv_color_white(), 0);
  lv_obj_set_style_text_font(g_msg, &lv_font_montserrat_20, 0);
  lv_obj_set_style_radius(g_msg, 20, 0);
  lv_obj_set_style_pad_hor(g_msg, 20, 0);
  lv_obj_set_style_pad_ver(g_msg, 10, 0);
  lv_obj_align(g_msg, LV_ALIGN_TOP_MID, 0, 14);
  lv_obj_add_flag(g_msg, LV_OBJ_FLAG_HIDDEN);

  // "No camera" overlay: rounded card shown when the UART link is silent.
  // Sits above the (blank) preview so bring-up isn't just a black screen.
  g_nocam = lv_obj_create(g_idle_scr);
  lv_obj_set_size(g_nocam, 440, 150);
  lv_obj_center(g_nocam);
  lv_obj_set_style_radius(g_nocam, 18, 0);
  lv_obj_set_style_bg_color(g_nocam, COL_CARD, 0);
  lv_obj_set_style_bg_opa(g_nocam, LV_OPA_90, 0);
  lv_obj_set_style_border_width(g_nocam, 0, 0);
  lv_obj_clear_flag(g_nocam, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* nw = lv_label_create(g_nocam);
  lv_label_set_text(nw, LV_SYMBOL_WARNING "  No camera connected");
  lv_obj_set_style_text_color(nw, COL_ACCENT, 0);
  lv_obj_set_style_text_font(nw, &lv_font_montserrat_20, 0);
  lv_obj_align(nw, LV_ALIGN_TOP_MID, 0, 22);
  lv_obj_t* nl = lv_label_create(g_nocam);
  lv_label_set_text(nl, "Check the UART link to the camera node");
  lv_obj_set_style_text_color(nl, COL_TEXT_DIM, 0);
  lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(nl, LV_ALIGN_TOP_MID, 0, 62);
  lv_obj_add_flag(g_nocam, LV_OBJ_FLAG_HIDDEN);  // hidden until proven missing

  // Bring-up diagnostic bar (top-left): UART frame counters + touch state.
  // The screen is the only reliable debug console on this board, so surface
  // the numbers here. Remove once the booth is verified end to end.
  g_diag = lv_label_create(g_idle_scr);
  lv_obj_set_style_bg_color(g_diag, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(g_diag, LV_OPA_50, 0);
  lv_obj_set_style_text_color(g_diag, lv_color_hex(0x00FF80), 0);
  lv_obj_set_style_radius(g_diag, 8, 0);
  lv_obj_set_style_pad_all(g_diag, 6, 0);
  lv_obj_align(g_diag, LV_ALIGN_TOP_LEFT, 6, 6);
  lv_label_set_text(g_diag, "link: --");
}

static void buildResultScreen() {
  g_result_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(g_result_scr, COL_BG_DARK, 0);

  lv_obj_t* title = lv_label_create(g_result_scr);
  lv_label_set_text(title, "Scan to get your photos");
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  // White rounded card behind the QR — doubles as the code's quiet zone.
  lv_obj_t* card = lv_obj_create(g_result_scr);
  lv_obj_set_size(card, kQrSize + 40, kQrSize + 40);
  lv_obj_align(card, LV_ALIGN_CENTER, 0, -26);
  lv_obj_set_style_radius(card, 16, 0);
  lv_obj_set_style_bg_color(card, lv_color_white(), 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_shadow_width(card, 30, 0);
  lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
  lv_obj_set_style_pad_all(card, 0, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  static lv_color_t* qrbuf = nullptr;
  if (!qrbuf)
    qrbuf = (lv_color_t*)ps_malloc(kQrSize * kQrSize * sizeof(lv_color_t));
  g_qr_canvas = lv_canvas_create(card);
  lv_canvas_set_buffer(g_qr_canvas, qrbuf, kQrSize, kQrSize,
                       LV_IMG_CF_TRUE_COLOR);
  lv_obj_center(g_qr_canvas);

  g_result_info = lv_label_create(g_result_scr);
  lv_obj_set_style_text_color(g_result_info, lv_color_white(), 0);
  lv_obj_set_style_text_font(g_result_info, &lv_font_montserrat_20, 0);
  lv_label_set_text(g_result_info, "");
  lv_obj_align(g_result_info, LV_ALIGN_CENTER, 0, 114);

  g_result_instr = lv_label_create(g_result_scr);
  lv_obj_set_style_text_color(g_result_instr, COL_TEXT_DIM, 0);
  lv_label_set_text(g_result_instr,
                    LV_SYMBOL_WIFI "  Join WiFi \"" AP_SSID "\", then scan");
  lv_obj_align(g_result_instr, LV_ALIGN_BOTTOM_MID, 0, -88);

  // Print progress line: swaps in for the WiFi hint while a job runs.
  g_print_status = lv_label_create(g_result_scr);
  lv_obj_set_style_text_color(g_print_status, COL_ACCENT, 0);
  lv_obj_set_style_text_font(g_print_status, &lv_font_montserrat_20, 0);
  lv_label_set_text(g_print_status, "");
  lv_obj_align(g_print_status, LV_ALIGN_BOTTOM_MID, 0, -90);
  lv_obj_add_flag(g_print_status, LV_OBJ_FLAG_HIDDEN);

  // Print (left) + Done (right), side by side.
  lv_obj_t* print = lv_btn_create(g_result_scr);
  lv_obj_set_size(print, 210, 64);
  lv_obj_align(print, LV_ALIGN_BOTTOM_MID, -115, -16);
  lv_obj_set_style_radius(print, 32, 0);
  lv_obj_set_style_bg_color(print, lv_color_hex(0x2B2B36), 0);
  lv_obj_set_style_shadow_width(print, 16, 0);
  lv_obj_set_style_shadow_opa(print, LV_OPA_30, 0);
  lv_obj_add_event_cb(print, printBtnCb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* pl = lv_label_create(print);
  lv_label_set_text(pl, LV_SYMBOL_FILE "  Print");
  lv_obj_set_style_text_font(pl, &lv_font_montserrat_20, 0);
  lv_obj_center(pl);

  lv_obj_t* done = lv_btn_create(g_result_scr);
  lv_obj_set_size(done, 210, 64);
  lv_obj_align(done, LV_ALIGN_BOTTOM_MID, 115, -16);
  lv_obj_set_style_radius(done, 32, 0);
  lv_obj_set_style_bg_color(done, COL_ACCENT, 0);
  lv_obj_set_style_shadow_width(done, 16, 0);
  lv_obj_set_style_shadow_opa(done, LV_OPA_30, 0);
  lv_obj_add_event_cb(done, doneBtnCb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* dl = lv_label_create(done);
  lv_label_set_text(dl, "Done");
  lv_obj_set_style_text_font(dl, &lv_font_montserrat_20, 0);
  lv_obj_center(dl);
}

// ---------------------------------------------------------------------------
void uiBegin() {
  // Preview frame buffer + descriptor.
  g_preview_px =
      (lv_color_t*)ps_malloc(PREVIEW_W * PREVIEW_H * sizeof(lv_color_t));
  g_preview_jpeg = (uint8_t*)ps_malloc(FRAME_MAX_PAYLOAD);
  memset(g_preview_px, 0, PREVIEW_W * PREVIEW_H * sizeof(lv_color_t));

  g_preview_dsc.header.always_zero = 0;
  g_preview_dsc.header.w = PREVIEW_W;
  g_preview_dsc.header.h = PREVIEW_H;
  g_preview_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  g_preview_dsc.data_size = PREVIEW_W * PREVIEW_H * sizeof(lv_color_t);
  g_preview_dsc.data = (const uint8_t*)g_preview_px;

  buildIdleScreen();
  buildResultScreen();
  uiShowIdle();
}

int uiSelectedShotCount() { return kShotCounts[g_selected_idx]; }

const char* uiActiveFilterFile() {
  return g_filter_idx == 0 ? "" : filtersFileName(g_filter_idx - 1);
}

void uiFiltersRefresh() {
  if (filtersCount() > 0) {
    lv_obj_clear_flag(g_arrow_l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_arrow_r, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(g_arrow_l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_arrow_r, LV_OBJ_FLAG_HIDDEN);
  }
}

bool uiTakePhotoRequested() {
  bool v = g_take_requested;
  g_take_requested = false;
  return v;
}

bool uiDoneRequested() {
  bool v = g_done_requested;
  g_done_requested = false;
  return v;
}

bool uiPrintRequested() {
  bool v = g_print_requested;
  g_print_requested = false;
  return v;
}

void uiSetPrintStatus(const char* s) {
  static char last[64] = "";
  if (strncmp(s, last, sizeof(last) - 1) == 0) return;  // no LVGL churn
  snprintf(last, sizeof(last), "%s", s);
  if (s[0]) {
    lv_label_set_text(g_print_status, s);
    lv_obj_clear_flag(g_print_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_result_instr, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(g_print_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_result_instr, LV_OBJ_FLAG_HIDDEN);
  }
}

void uiShowIdle() {
  uiHideCountdown();
  uiSetFlash(false);
  g_preview_frozen = false;  // never come back to idle with a stuck preview
  lv_obj_add_flag(g_msg, LV_OBJ_FLAG_HIDDEN);
  lv_scr_load(g_idle_scr);
}

void uiShowResult(const char* sessionId, int photoCount) {
  if (cloudReady()) {
    // Booth is online: the QR is a public https link opened over the
    // guest's own mobile data — nobody joins the booth WiFi.
    char curl[96];
    cloudGalleryUrl(sessionId, curl, sizeof(curl));
    qrRenderToCanvas(g_qr_canvas, curl, kQrSize);
    lv_label_set_text_fmt(g_result_info, "%d photo%s  -  %s", photoCount,
                          photoCount == 1 ? "" : "s", curl);
    lv_label_set_text(g_result_instr,
                      LV_SYMBOL_IMAGE "  Opens on your own internet");
  } else {
    // Offline fallback: guest joins the booth WiFi by hand, then the QR is a
    // direct link to this session's gallery.
    char url[48];
    qrBuildUrl(sessionId, url, sizeof(url));
    qrRenderToCanvas(g_qr_canvas, url, kQrSize);
    lv_label_set_text_fmt(g_result_info, "%d photo%s  -  %s", photoCount,
                          photoCount == 1 ? "" : "s", url);
    lv_label_set_text_fmt(g_result_instr,
                          LV_SYMBOL_WIFI "  Join WiFi \"%s\", then scan",
                          AP_SSID);
  }
  lv_scr_load(g_result_scr);
}

// Pop animation for the countdown badge: starts slightly oversized and eases
// back to normal each second, so the tick feels alive.
static void countdownZoomCb(void* obj, int32_t v) {
  lv_obj_set_style_transform_zoom((lv_obj_t*)obj, (lv_coord_t)v, 0);
}

void uiShowCountdown(int n) {
  lv_obj_add_flag(g_msg, LV_OBJ_FLAG_HIDDEN);  // clear "Get ready!" etc.
  lv_obj_clear_flag(g_count_circle, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text_fmt(g_countdown_lbl, "%d", n);

  // Zoom around the badge's center (it is a fixed 200x200 circle).
  lv_obj_set_style_transform_pivot_x(g_count_circle, 100, 0);
  lv_obj_set_style_transform_pivot_y(g_count_circle, 100, 0);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, g_count_circle);
  lv_anim_set_exec_cb(&a, countdownZoomCb);
  lv_anim_set_values(&a, 330, 256);  // 256 = 1.0x
  lv_anim_set_time(&a, 300);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

void uiHideCountdown() {
  if (g_count_circle) lv_obj_add_flag(g_count_circle, LV_OBJ_FLAG_HIDDEN);
}

void uiSetFlash(bool on) {
  if (on)
    lv_obj_clear_flag(g_flash, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(g_flash, LV_OBJ_FLAG_HIDDEN);
}

void uiShowMessage(const char* msg) {
  lv_label_set_text(g_msg, msg);
  lv_obj_clear_flag(g_msg, LV_OBJ_FLAG_HIDDEN);
}
