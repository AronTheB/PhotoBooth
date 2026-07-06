// =============================================================================
// uart_link.cpp  —  see uart_link.h
// =============================================================================
#include "uart_link.h"

#include "app.h"
#include "photobooth_protocol.h"

static HardwareSerial& Link = Serial1;
static FrameParser g_parser;

// --- Latest preview frame (single-slot, newest wins) ---
static uint8_t* g_preview_buf = nullptr;
static size_t g_preview_len = 0;
static volatile bool g_preview_new = false;
static SemaphoreHandle_t g_preview_mtx = nullptr;

// --- Pending capture (full-res still) ---
static uint8_t* g_capture_buf = nullptr;
static size_t g_capture_len = 0;
static volatile bool g_capture_ready = false;
static SemaphoreHandle_t g_capture_mtx = nullptr;

// --- TX guard (commands can be sent from the UI core) ---
static SemaphoreHandle_t g_tx_mtx = nullptr;

// --- Link liveness: millis() of the last valid frame (0 = none yet) ---
static volatile uint32_t g_last_rx_ms = 0;

// --- Raw byte counter (diagnostics) ---
static volatile uint32_t g_bytes_rx = 0;

// ---------------------------------------------------------------------------
static void onFrame(uint8_t type, const uint8_t* payload, uint32_t len) {
  // Any valid (CRC-checked) frame proves the camera node is alive on the wire.
  g_last_rx_ms = millis();

  switch (type) {
    case MSG_PREVIEW_FRAME:
      if (xSemaphoreTake(g_preview_mtx, 0) == pdTRUE) {
        if (len <= FRAME_MAX_PAYLOAD) {
          memcpy(g_preview_buf, payload, len);
          g_preview_len = len;
          g_preview_new = true;
        }
        xSemaphoreGive(g_preview_mtx);
      }
      // If we couldn't take the mutex, the UI is mid-copy; just drop this
      // preview frame — freshness matters more than any single frame.
      break;

    case MSG_CAPTURE_FRAME:
      if (xSemaphoreTake(g_capture_mtx, portMAX_DELAY) == pdTRUE) {
        if (len <= FRAME_MAX_PAYLOAD) {
          memcpy(g_capture_buf, payload, len);
          g_capture_len = len;
          g_capture_ready = true;
        }
        xSemaphoreGive(g_capture_mtx);
      }
      break;

    case MSG_ACK:
    case MSG_ERR:
      // Logged for bring-up; the session state machine drives capture timing
      // off CAPTURE_FRAME arrival + timeout rather than ACK.
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
static void uartTask(void* arg) {
  uint8_t chunk[512];
  for (;;) {
    int n = Link.available();
    if (n > 0) {
      if (n > (int)sizeof(chunk)) n = sizeof(chunk);
      int got = Link.readBytes(chunk, n);
      g_bytes_rx += got;
      g_parser.feed(chunk, got);
    } else {
      vTaskDelay(1);  // nothing waiting; yield a tick
    }
  }
}

// ---------------------------------------------------------------------------
void uartLinkBegin() {
  g_preview_mtx = xSemaphoreCreateMutex();
  g_capture_mtx = xSemaphoreCreateMutex();
  g_tx_mtx = xSemaphoreCreateMutex();

  g_preview_buf = (uint8_t*)ps_malloc(FRAME_MAX_PAYLOAD);
  g_capture_buf = (uint8_t*)ps_malloc(FRAME_MAX_PAYLOAD);

  Link.setRxBufferSize(32 * 1024);  // room to absorb a full-res still burst
  Link.begin(UART_BAUD, SERIAL_8N1, VIEWE_UART_RX_PIN, VIEWE_UART_TX_PIN);

  g_parser.begin();
  g_parser.onFrame(onFrame);

  // Receiver on core 0, away from LVGL (core 1). Generous stack for the memcpy.
  xTaskCreatePinnedToCore(uartTask, "uartRx", 4096, nullptr, 2, nullptr, 0);
}

// ---------------------------------------------------------------------------
size_t uartTakeLatestPreview(uint8_t* dst, size_t cap) {
  size_t out = 0;
  if (xSemaphoreTake(g_preview_mtx, 0) != pdTRUE) return 0;
  if (g_preview_new && g_preview_len <= cap) {
    memcpy(dst, g_preview_buf, g_preview_len);
    out = g_preview_len;
    g_preview_new = false;
  }
  xSemaphoreGive(g_preview_mtx);
  return out;
}

bool uartCaptureReady() { return g_capture_ready; }

size_t uartConsumeCapture(uint8_t* dst, size_t cap) {
  size_t out = 0;
  if (xSemaphoreTake(g_capture_mtx, portMAX_DELAY) != pdTRUE) return 0;
  if (g_capture_ready && g_capture_len <= cap) {
    memcpy(dst, g_capture_buf, g_capture_len);
    out = g_capture_len;
  }
  g_capture_ready = false;
  xSemaphoreGive(g_capture_mtx);
  return out;
}

void uartClearCapture() {
  if (xSemaphoreTake(g_capture_mtx, portMAX_DELAY) == pdTRUE) {
    g_capture_ready = false;
    xSemaphoreGive(g_capture_mtx);
  }
}

// ---------------------------------------------------------------------------
static void txCommand(uint8_t type) {
  if (xSemaphoreTake(g_tx_mtx, portMAX_DELAY) == pdTRUE) {
    writeCommand(Link, type);
    xSemaphoreGive(g_tx_mtx);
  }
}

void uartSendStartPreview() { txCommand(MSG_CMD_START_PREVIEW); }
void uartSendStopPreview() { txCommand(MSG_CMD_STOP_PREVIEW); }
void uartSendCapture() { txCommand(MSG_CMD_CAPTURE); }

uint32_t uartMsSinceLastFrame() {
  uint32_t last = g_last_rx_ms;
  if (last == 0) return UINT32_MAX;  // never seen a frame
  return millis() - last;
}

uint32_t uartFramesOk() { return g_parser.framesOk(); }
uint32_t uartFramesBadCrc() { return g_parser.framesBadCrc(); }
uint32_t uartBytesRx() { return g_bytes_rx; }
