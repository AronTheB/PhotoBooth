// =============================================================================
// session.cpp  —  see session.h. Implements the state machine in build plan §7:
//
//   IDLE -> COUNTDOWN -> CAPTURE -> (more? INTERSHOT_WAIT -> COUNTDOWN)
//                                -> (done? PROCESS -> RESULT -> IDLE)
// =============================================================================
#include "session.h"

#include <Arduino.h>

#include "app.h"
#include "cloud.h"
#include "display_bsp.h"
#include "printer.h"
#include "storage.h"
#include "uart_link.h"
#include "ui.h"
#include "web_server.h"

enum State : uint8_t {
  ST_IDLE,
  ST_COUNTDOWN,
  ST_CAPTURE,
  ST_INTERSHOT,
  ST_PROCESS,
  ST_RESULT,
};

static State g_state = ST_IDLE;

static char g_session_id[SESSION_ID_LEN + 1];
static int g_shot_count = 1;   // how many shots this session
static int g_shot_index = 0;   // shots already captured

// Countdown bookkeeping.
static int g_count_val = 0;
static uint32_t g_count_tick = 0;  // millis of last decrement

// Capture bookkeeping.
static uint32_t g_capture_sent = 0;
static int g_capture_tries = 0;
static uint8_t* g_capture_buf = nullptr;  // holds the received still

// Inter-shot / result timers.
static uint32_t g_intershot_start = 0;
static uint32_t g_result_start = 0;

// ---------------------------------------------------------------------------
void sessionBegin() {
  g_capture_buf = (uint8_t*)ps_malloc(FRAME_MAX_PAYLOAD);
  g_state = ST_IDLE;
  uartSendStartPreview();  // make sure the camera is streaming
}

// --- Enter COUNTDOWN with a starting value (5 first shot, 3 subsequent) ---
static void startCountdown(int from) {
  g_count_val = from;
  g_count_tick = millis();
  uiShowCountdown(g_count_val);
  g_state = ST_COUNTDOWN;
}

// --- Begin the capture: freeze the preview, flash on, fire CMD_CAPTURE ---
static void beginCapture() {
  uiHideCountdown();
  uiFreezePreview(true);  // hold the last live frame while the still is taken
  uiSetFlash(true);
  displaySetBacklight(255);  // push backlight for the flash
  uartClearCapture();
  uartSendCapture();
  g_capture_sent = millis();
  g_capture_tries = 1;
  g_state = ST_CAPTURE;
}

// --- After a shot is saved, decide whether to shoot again or finish ---
static void afterShot() {
  uiSetFlash(false);
  uiFreezePreview(false);
  if (g_shot_index < g_shot_count) {
    uiShowMessage("Get ready!");
    g_intershot_start = millis();
    g_state = ST_INTERSHOT;
  } else {
    g_state = ST_PROCESS;
  }
}

// ---------------------------------------------------------------------------
void sessionLoop() {
  uint32_t now = millis();

  // Preview keepalive: the camera only streams while it keeps hearing this,
  // so it goes quiet when this board is off or being flashed (GPIO43 is
  // shared with the USB flashing bridge). Repeat in every state that shows a
  // live preview.
  static uint32_t last_keepalive = 0;
  bool want_preview = g_state == ST_IDLE || g_state == ST_COUNTDOWN ||
                      g_state == ST_INTERSHOT;
  if (want_preview && now - last_keepalive >= PREVIEW_KEEPALIVE_MS) {
    last_keepalive = now;
    uartSendStartPreview();
  }

  switch (g_state) {
    case ST_IDLE:
      if (uiTakePhotoRequested()) {
        if (!storageNewSession(g_session_id)) {
          uiShowMessage("SD error - cannot start");
          return;
        }
        g_shot_count = uiSelectedShotCount();
        g_shot_index = 0;
        Serial.printf("[session] start %s x%d\n", g_session_id, g_shot_count);
        startCountdown(COUNTDOWN_START);
      }
      break;

    case ST_COUNTDOWN:
      if (now - g_count_tick >= 1000) {
        g_count_tick += 1000;
        g_count_val--;
        if (g_count_val > 0) {
          uiShowCountdown(g_count_val);
        } else {
          beginCapture();
        }
      }
      break;

    case ST_CAPTURE:
      if (uartCaptureReady()) {
        size_t len = uartConsumeCapture(g_capture_buf, FRAME_MAX_PAYLOAD);
        if (len > 0) {
          g_shot_index++;
          if (!storageSaveJpeg(g_session_id, g_shot_index, g_capture_buf,
                               len)) {
            uiShowMessage("SD write failed");
          }
          // Remember the filter that was on screen so the gallery can burn
          // it into the downloaded image.
          storageSaveFilterTag(g_session_id, g_shot_index,
                               uiActiveFilterFile());
          afterShot();
        }
      } else if (now - g_capture_sent >= CAPTURE_TIMEOUT_MS) {
        if (g_capture_tries < CAPTURE_RETRIES) {
          Serial.println("[session] capture timeout, retrying");
          uartClearCapture();
          uartSendCapture();
          g_capture_sent = now;
          g_capture_tries++;
        } else {
          Serial.println("[session] capture failed, aborting");
          uiShowMessage("Capture failed");
          uiSetFlash(false);
          uiFreezePreview(false);
          g_result_start = now;   // brief error dwell, then home
          g_state = ST_RESULT;
        }
      }
      break;

    case ST_INTERSHOT:
      if (now - g_intershot_start >= (uint32_t)INTERSHOT_SECONDS * 1000) {
        startCountdown(COUNTDOWN_SHORT);
      }
      break;

    case ST_PROCESS:
      // Gallery HTML is generated on-demand by the web server, and the QR is
      // built when we show the result screen — so there is nothing heavy to do
      // here beyond transitioning.
      webServerSetActiveSession(g_session_id);  // captive portal lands here
      if (cloudReady())  // public https gallery when the booth is online
        cloudUploadSession(g_session_id, g_shot_index);
      uiShowResult(g_session_id, g_shot_index);
      g_result_start = now;
      g_state = ST_RESULT;
      break;

    case ST_RESULT:
      // Kick off thermal printing on request; hold the screen while it runs.
      if (uiPrintRequested() && !printerBusy()) {
        printerStartJob(g_session_id, g_shot_index);
      }
      uiSetPrintStatus(printerStatus());
      if (printerBusy()) g_result_start = now;  // no timeout mid-print

      if ((uiDoneRequested() && !printerBusy()) ||
          now - g_result_start >= (uint32_t)RESULT_TIMEOUT_S * 1000) {
        uiSetPrintStatus("");
        uiShowIdle();
        uartSendStartPreview();  // ensure preview resumes for the next guest
        g_state = ST_IDLE;
      }
      break;
  }
}
