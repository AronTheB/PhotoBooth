// =============================================================================
// camera-node / main.cpp  —  XIAO ESP32-S3 Sense firmware (build plan §5)
//
// Responsibilities:
//   * Init the OV2640 with PSRAM frame buffers, JPEG output.
//   * PREVIEW state: capture small JPEGs, send as PREVIEW_FRAME as fast as the
//     UART allows. DROP frames rather than block the capture loop. Streaming
//     is keepalive-gated: it stops when the display goes silent, so the XIAO
//     never jams the VIEWE's shared GPIO43 boot/flash line during uploads.
//   * On CMD_CAPTURE: stop preview, switch to full-res, grab one still, send it
//     as CAPTURE_FRAME + ACK, then resume preview.
//   * Honor CMD_STOP_PREVIEW / CMD_START_PREVIEW.
//
// The single OV2640 cannot preview and grab a full-res still at once, so the
// capture sequence briefly freezes preview — which is fine, the display is
// flashing white at that moment (build plan §3).
// =============================================================================
#include <Arduino.h>
#include "esp_camera.h"

#include "camera_pins.h"
#include "photobooth_protocol.h"

// The UART link to the VIEWE board. Serial1 with the XIAO's D6/D7 pins.
static HardwareSerial& Link = Serial1;

// Parser for inbound commands from the VIEWE.
static FrameParser g_parser;

// Simple two-state machine (build plan §5).
enum CamState : uint8_t { STATE_PREVIEW, STATE_IDLE };
static volatile CamState g_state = STATE_IDLE;

// millis() of the last valid inbound command. The display refreshes preview
// with CMD_START_PREVIEW every PREVIEW_KEEPALIVE_MS; if this goes stale we
// stop streaming (the display is off, rebooting, or being flashed).
static uint32_t g_last_cmd_ms = 0;

// The camera is initialized at STILL_FRAMESIZE so its frame buffers are large
// enough; for preview we scale the sensor down at runtime with set_framesize().
static bool g_at_still_res = true;

// Bring-up telemetry: how many preview frames the sensor produced / we sent,
// printed once a second so the USB serial shows the node is alive and the
// camera is actually delivering frames.
static uint32_t g_frames_grabbed = 0;  // esp_camera_fb_get() succeeded
static uint32_t g_frames_sent = 0;     // actually written to the UART
static uint32_t g_grab_fails = 0;      // esp_camera_fb_get() returned null
static uint32_t g_last_status_ms = 0;

// ---------------------------------------------------------------------------
// Camera init
// ---------------------------------------------------------------------------
static bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  // Init at the LARGER (still) resolution so the driver allocates big enough
  // buffers; we drop the sensor to preview size at runtime.
  config.frame_size = STILL_FRAMESIZE;
  config.jpeg_quality = STILL_JPEG_QUALITY;
  config.fb_count = 2;                    // double-buffer for smoother preview
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;  // always hand us the freshest frame

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[cam] esp_camera_init failed: 0x%x\n", err);
    return false;
  }

  // OV2640 image-quality tuning. The driver defaults leave the corrections
  // off/neutral; these visibly clean up the image on the Sense module.
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_lenc(s, 1);        // lens shading correction (dark corners)
    s->set_bpc(s, 1);         // black pixel correction
    s->set_wpc(s, 1);         // white pixel correction
    s->set_dcw(s, 1);         // downsize crop/window (cleaner scaling)
    s->set_saturation(s, 1);  // -2..2; defaults look washed out
  }
  return true;
}

// Switch the sensor between preview (small/fast) and still (large/quality).
static void setResolution(bool still) {
  if (still == g_at_still_res) return;
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  if (still) {
    s->set_framesize(s, STILL_FRAMESIZE);
    s->set_quality(s, STILL_JPEG_QUALITY);
  } else {
    s->set_framesize(s, PREVIEW_FRAMESIZE);
    s->set_quality(s, PREVIEW_JPEG_QUALITY);
  }
  g_at_still_res = still;
  // The OV2640 needs a couple of frames to settle after a framesize change;
  // discard them so we don't send a half-configured frame.
  for (int i = 0; i < 2; ++i) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
  }
}

// ---------------------------------------------------------------------------
// Full-res capture sequence (response to CMD_CAPTURE)
// ---------------------------------------------------------------------------
static void doCapture() {
  g_state = STATE_IDLE;      // stop the preview loop
  setResolution(true);       // switch sensor up to full-res

  // Let auto-exposure/AWB re-converge at the new resolution: the first frames
  // after a mode switch come out over/under-exposed. The display freezes the
  // preview during this, so the extra ~500 ms is invisible to the guest.
  for (int i = 0; i < 3; ++i) {
    camera_fb_t* skip = esp_camera_fb_get();
    if (skip) esp_camera_fb_return(skip);
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[cam] capture fb_get failed");
    writeByteFrame(Link, MSG_ERR, 0x01);  // 0x01 = capture failed
  } else {
    // Send the still. This is a one-shot, so blocking until it's all out is
    // correct here (unlike the preview loop).
    writeFrame(Link, MSG_CAPTURE_FRAME, fb->buf, fb->len);
    writeByteFrame(Link, MSG_ACK, MSG_CAPTURE_FRAME);
    esp_camera_fb_return(fb);
    Serial.printf("[cam] sent still %u bytes\n", (unsigned)fb->len);
  }

  setResolution(false);      // back to preview size
  g_state = STATE_PREVIEW;   // resume streaming
}

// ---------------------------------------------------------------------------
// Command handling (frames arriving from the VIEWE)
// ---------------------------------------------------------------------------
static void onCommand(uint8_t type, const uint8_t* payload, uint32_t len) {
  g_last_cmd_ms = millis();
  switch (type) {
    case MSG_CMD_START_PREVIEW:
      g_state = STATE_PREVIEW;
      break;
    case MSG_CMD_STOP_PREVIEW:
      g_state = STATE_IDLE;
      break;
    case MSG_CMD_CAPTURE:
      // Optional 1-byte resolution enum in payload could override STILL_* here.
      doCapture();
      break;
    default:
      // Unknown/unsupported command from this side — ignore.
      break;
  }
}

// ---------------------------------------------------------------------------
// Preview: grab one small JPEG and send it, dropping if the UART is backed up.
// ---------------------------------------------------------------------------
static void sendPreviewFrame() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    g_grab_fails++;
    return;
  }
  g_frames_grabbed++;

  // Drop-rather-than-block: only send if the whole frame fits in the TX buffer
  // right now. If not, discard this frame and grab a fresher one next loop.
  size_t needed = fb->len + FRAME_OVERHEAD;
  if ((size_t)Link.availableForWrite() >= needed) {
    writeFrame(Link, MSG_PREVIEW_FRAME, fb->buf, fb->len);
    g_frames_sent++;
  }
  esp_camera_fb_return(fb);
}

// Print a one-line health summary once a second (bring-up aid).
static void printStatus() {
  uint32_t now = millis();
  if (now - g_last_status_ms < 1000) return;
  g_last_status_ms = now;
  Serial.printf("[cam] state=%s grabbed=%u sent=%u grabFails=%u\n",
                g_state == STATE_PREVIEW ? "PREVIEW" : "IDLE",
                (unsigned)g_frames_grabbed, (unsigned)g_frames_sent,
                (unsigned)g_grab_fails);
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[cam] XIAO ESP32-S3 photo booth camera node");

  // UART link. Give the TX buffer room for a whole preview frame so writeFrame
  // never actually blocks in the preview loop (we gate on availableForWrite).
  // Must exceed the largest q10 QVGA frame, or the gate drops every frame.
  Link.setTxBufferSize(32 * 1024);
  Link.setRxBufferSize(2 * 1024);
  Link.begin(UART_BAUD, SERIAL_8N1, XIAO_UART_RX_PIN, XIAO_UART_TX_PIN);

  if (!g_parser.begin()) {
    Serial.println("[cam] FrameParser alloc failed (PSRAM?)");
  }
  g_parser.onFrame(onCommand);

  if (!initCamera()) {
    Serial.println("[cam] camera init failed — halting");
    while (true) delay(1000);
  }

  setResolution(false);  // start in preview resolution
  g_state = STATE_IDLE;  // silent until the display sends its first keepalive
  Serial.println("[cam] ready, waiting for display");
}

void loop() {
  // 1) Drain inbound command bytes into the frame parser.
  while (Link.available()) {
    g_parser.feed((uint8_t)Link.read());
  }

  // 2) Keepalive gate: if the display has gone silent, stop transmitting so
  //    the shared GPIO43 net is free (e.g. for esptool flashing the VIEWE).
  if (g_state == STATE_PREVIEW && millis() - g_last_cmd_ms > PREVIEW_STALE_MS) {
    Serial.println("[cam] display silent, pausing preview");
    g_state = STATE_IDLE;
  }

  // 3) Stream preview when in PREVIEW state.
  if (g_state == STATE_PREVIEW) {
    sendPreviewFrame();
  } else {
    delay(1);  // idle; yield so command handling stays responsive
  }

  // 4) Heartbeat to USB serial so bring-up can see the node is alive.
  printStatus();
}
