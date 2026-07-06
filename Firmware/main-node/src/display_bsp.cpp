// =============================================================================
// display_bsp.cpp  —  see display_bsp.h
//
// !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
//  EVERY PIN AND TIMING CONSTANT IN THIS FILE IS A PLACEHOLDER.
//  Copy the real values from the VIEWE 4.3" demo/BSP for your exact model
//  (the RGB timing block, the 16 data pins, DE/VSYNC/HSYNC/PCLK, the backlight
//  pin, and the GT911 touch I2C pins + reset/int). Wrong values = white/garbled
//  screen or no touch. This is build plan §2's "confirm the pinout" item.
// !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
// =============================================================================
#include "display_bsp.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <TAMC_GT911.h>
#include <Wire.h>

#include "app.h"

// ---- TODO: panel control + data pins (from VIEWE demo) ----------------------
#define LCD_DE 40
#define LCD_VSYNC 41
#define LCD_HSYNC 39
#define LCD_PCLK 42
#define LCD_BL 2  // backlight enable / PWM

// 16-bit RGB565 data bus (R0..R4, G0..G5, B0..B4).
static const int8_t LCD_R_PINS[5] = {45, 48, 47, 21, 14};
static const int8_t LCD_G_PINS[6] = {5, 6, 7, 15, 16, 4};
static const int8_t LCD_B_PINS[5] = {8, 3, 46, 9, 1};

// ---- TODO: RGB timing (from VIEWE demo) — for an 800x480 panel --------------
// Each axis needs: sync polarity, front porch, pulse width, back porch.
static const uint16_t H_POL = 0, H_FP = 8, H_PW = 4, H_BP = 8;
static const uint16_t V_POL = 0, V_FP = 8, V_PW = 4, V_BP = 8;
static const uint16_t PCLK_ACTIVE_NEG = 1;      // sample on falling edge (typ.)
static const int32_t PCLK_SPEED_HZ = 14000000;  // ~14 MHz pixel clock

// ---- GT911 capacitive touch (I2C) — VIEWE UEDX80480043E-WB-A ----------------
// Confirmed against VIEWE's board docs for this exact model.
#define TOUCH_SDA 19
#define TOUCH_SCL 20
#define TOUCH_INT 18  // VIEWE wires INT to GPIO18 (was -1 -> "Invalid pin")
#define TOUCH_RST 38
#define TOUCH_ADDR GT911_ADDR1

// ---------------------------------------------------------------------------
static Arduino_ESP32RGBPanel* g_rgbpanel = nullptr;
static Arduino_RGB_Display* g_gfx = nullptr;
static TAMC_GT911 g_touch(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST, DISP_WIDTH,
                          DISP_HEIGHT);

// LVGL draw buffers (in PSRAM). Two partial buffers of 1/10 screen each is a
// good responsiveness/RAM trade for an RGB panel.
static lv_disp_draw_buf_t g_draw_buf;
static lv_color_t* g_buf1 = nullptr;
static lv_color_t* g_buf2 = nullptr;
static lv_disp_drv_t g_disp_drv;
static lv_indev_drv_t g_indev_drv;

// --- LVGL flush: blit the rendered area to the panel ---
static void flushCb(lv_disp_drv_t* drv, const lv_area_t* area,
                    lv_color_t* px) {
  int w = area->x2 - area->x1 + 1;
  int h = area->y2 - area->y1 + 1;
  g_gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)px, w, h);
  lv_disp_flush_ready(drv);
}

// --- LVGL touch read ---
static uint32_t g_touch_count = 0;   // total PRESSED reports since boot
static int16_t g_touch_x = -1, g_touch_y = -1;  // last reported point

static void touchCb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  g_touch.read();
  if (g_touch.isTouched) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = g_touch.points[0].x;
    data->point.y = g_touch.points[0].y;
    g_touch_count++;
    g_touch_x = g_touch.points[0].x;
    g_touch_y = g_touch.points[0].y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

uint32_t displayTouchCount() { return g_touch_count; }
void displayLastTouch(int16_t* x, int16_t* y) {
  *x = g_touch_x;
  *y = g_touch_y;
}

// LVGL needs a millisecond tick. Drive it from the Arduino millis via a timer.
static void lvTickTask(void* arg) {
  (void)arg;
  for (;;) {
    lv_tick_inc(2);
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ---------------------------------------------------------------------------
bool displayBspBegin() {
  g_rgbpanel = new Arduino_ESP32RGBPanel(
      LCD_DE, LCD_VSYNC, LCD_HSYNC, LCD_PCLK, LCD_R_PINS[0], LCD_R_PINS[1],
      LCD_R_PINS[2], LCD_R_PINS[3], LCD_R_PINS[4], LCD_G_PINS[0], LCD_G_PINS[1],
      LCD_G_PINS[2], LCD_G_PINS[3], LCD_G_PINS[4], LCD_G_PINS[5], LCD_B_PINS[0],
      LCD_B_PINS[1], LCD_B_PINS[2], LCD_B_PINS[3], LCD_B_PINS[4],
      H_POL, H_FP, H_PW, H_BP, V_POL, V_FP, V_PW, V_BP, PCLK_ACTIVE_NEG,
      PCLK_SPEED_HZ);
  g_gfx = new Arduino_RGB_Display(DISP_WIDTH, DISP_HEIGHT, g_rgbpanel);

  if (!g_gfx->begin()) {
    Serial.println("[disp] gfx begin failed");
    return false;
  }
  g_gfx->fillScreen(BLACK);

  pinMode(LCD_BL, OUTPUT);
  displaySetBacklight(255);

  // Touch. NB: TAMC_GT911's ROTATION_NORMAL flips both axes; on this panel the
  // raw GT911 coords already match the display, so INVERTED (= raw) is correct.
  // Verified on hardware: with NORMAL, a top-right press landed bottom-left.
  g_touch.begin();
  g_touch.setRotation(ROTATION_INVERTED);

  // LVGL.
  lv_init();
  const size_t buf_px = DISP_WIDTH * DISP_HEIGHT / 10;
  g_buf1 = (lv_color_t*)ps_malloc(buf_px * sizeof(lv_color_t));
  g_buf2 = (lv_color_t*)ps_malloc(buf_px * sizeof(lv_color_t));
  if (!g_buf1 || !g_buf2) {
    Serial.println("[disp] LVGL draw buffer alloc failed");
    return false;
  }
  lv_disp_draw_buf_init(&g_draw_buf, g_buf1, g_buf2, buf_px);

  lv_disp_drv_init(&g_disp_drv);
  g_disp_drv.hor_res = DISP_WIDTH;
  g_disp_drv.ver_res = DISP_HEIGHT;
  g_disp_drv.flush_cb = flushCb;
  g_disp_drv.draw_buf = &g_draw_buf;
  lv_disp_drv_register(&g_disp_drv);

  lv_indev_drv_init(&g_indev_drv);
  g_indev_drv.type = LV_INDEV_TYPE_POINTER;
  g_indev_drv.read_cb = touchCb;
  lv_indev_drv_register(&g_indev_drv);

  // Tick source.
  xTaskCreatePinnedToCore(lvTickTask, "lvTick", 2048, nullptr, 1, nullptr, 1);

  Serial.println("[disp] LVGL + panel ready");
  return true;
}

void displaySetBacklight(uint8_t level) {
  // Simple on/off if the pin is a plain enable; swap for ledc PWM if the board
  // supports brightness control.
  digitalWrite(LCD_BL, level > 0 ? HIGH : LOW);
}
