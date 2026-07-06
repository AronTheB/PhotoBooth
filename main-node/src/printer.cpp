// =============================================================================
// printer.cpp  —  see printer.h
// =============================================================================
#include "printer.h"

#include <JPEGDEC.h>
#include <NimBLEDevice.h>
#include <SD.h>

#include "app.h"
#include "storage.h"
#include "web_server.h"

// ---- Print geometry ---------------------------------------------------------
static const int kW = PRINTER_DOTS;       // dots across the paper (384)
static const int kL = PRINTER_PRINT_LEN;  // dots along the paper (640)
static const int kBytesPerLine = kW / 8;  // 48
static const int kStripLines = 80;        // lines per ESC/POS raster command

// Decoded photo geometry (UXGA still at half scale).
static const int kRgbW = 800, kRgbH = 600;

// ---- Job state ---------------------------------------------------------------
static TaskHandle_t s_task = nullptr;
static volatile bool s_busy = false;
static char s_status[64] = "";
static char s_session[SESSION_ID_LEN + 1] = "";
static volatile int s_count = 0;

// ---- Work buffers (PSRAM, allocated on first job) ----------------------------
static JPEGDEC s_jpeg;
static uint8_t* s_jpg = nullptr;   // raw JPEG from SD
static uint16_t* s_rgb = nullptr;  // decoded RGB565, kRgbW x kRgbH
static uint8_t* s_gray = nullptr;  // rotated/cropped grayscale, kW x kL
static int s_dec_w = 0, s_dec_h = 0;

// ---- BLE ---------------------------------------------------------------------
static NimBLEClient* s_cli = nullptr;
static NimBLERemoteCharacteristic* s_chr = nullptr;
static NimBLEAdvertisedDevice s_dev;

// Prefer acknowledged writes: write-without-response has NO flow control and
// the printer's internal BLE->MCU bridge drops bytes once a few KB stream in
// (verified: multi-strip test prints lost whole chunks mid-job). Acknowledged
// writes are paced by the printer itself and cannot silently lose data.
static bool s_ackWrites = true;

static void setStatus(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_status, sizeof(s_status), fmt, ap);
  va_end(ap);
  Serial.printf("[print] %s\n", s_status);
}

// ---------------------------------------------------------------------------
// BLE: scan for the printer by name prefix, connect, find a writable char.
// The link is kept open between jobs; a watchdog in the task reconnects.
// ---------------------------------------------------------------------------
static bool s_dev_valid = false;  // s_dev holds a printer seen in a past scan

static bool printerLinked() {
  return s_cli && s_cli->isConnected() && s_chr;
}

static bool bleConnectAttempts(int attempts) {
  if (!s_cli) s_cli = NimBLEDevice::createClient();
  s_cli->setConnectTimeout(10);
  // Wide initiating scan window (last two args, 0.625ms units) — with WiFi
  // sharing the radio, the default 10ms window routinely misses the
  // printer's connectable advertisements (HCI 0x3E / status 574).
  s_cli->setConnectionParams(12, 24, 0, 200, 80, 80);
  for (int a = 1; a <= attempts; ++a) {
    if (s_cli->connect(&s_dev)) return true;
    Serial.printf("[print] connect attempt %d failed\n", a);
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  return false;
}

// Find a writable characteristic on the connected printer. Prefer
// write-without-response capable chars for detection, but transfers use
// acknowledged writes (see bleWrite).
static bool findWriteChar() {
  s_chr = nullptr;
  std::vector<NimBLERemoteService*>* svcs = s_cli->getServices(true);
  if (svcs) {
    for (auto* sv : *svcs) {
      std::vector<NimBLERemoteCharacteristic*>* chs =
          sv->getCharacteristics(true);
      if (!chs) continue;
      for (auto* ch : *chs) {
        if (ch->canWriteNoResponse()) {
          s_chr = ch;
          break;
        }
        if (!s_chr && ch->canWrite()) s_chr = ch;
      }
      if (s_chr && s_chr->canWriteNoResponse()) break;
    }
  }
  if (!s_chr) return false;
  s_ackWrites = s_chr->canWrite();
  Serial.printf("[print] using char %s (ack=%d noRsp=%d)\n",
                s_chr->getUUID().toString().c_str(), (int)s_ackWrites,
                (int)s_chr->canWriteNoResponse());
  return true;
}

static bool bleConnect(bool verbose) {
  // Fast path: skip the 10s scan when the printer is already known from a
  // previous scan — this is what makes reconnects (and boot-time connects
  // after the first) quick.
  if (s_dev_valid) {
    if (bleConnectAttempts(4) && findWriteChar()) return true;
    if (s_cli && s_cli->isConnected()) s_cli->disconnect();
  }

  if (verbose) setStatus("Looking for printer...");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);   // request scan responses (names often live there)
  scan->setInterval(100);      // near-continuous window so WiFi coexistence
  scan->setWindow(99);         // doesn't make us miss advertisements
  scan->setMaxResults(0xFF);
  NimBLEScanResults res = scan->start(10, false);

  Serial.printf("[print] scan done, %d device(s):\n", res.getCount());
  bool found = false;
  int bestRssi = -128;
  for (int i = 0; i < res.getCount(); ++i) {
    NimBLEAdvertisedDevice d = res.getDevice(i);
    std::string name = d.getName();
    // Log EVERYTHING — a printer with no BLE name still shows by address.
    // advType 0/1 = connectable, 2/3 = scan-only/beacon (NOT connectable —
    // if the printer reports 2 or 3, its data path is Classic-BT only and no
    // ESP32-S3 can ever print to it).
    Serial.printf("[print]   %s rssi=%d advType=%d name='%s'\n",
                  d.getAddress().toString().c_str(), d.getRSSI(),
                  (int)d.getAdvType(), name.c_str());
    // Among all prefix matches, take the strongest signal: venues can have
    // several identically-named Xprinters in range, and ours is the nearby
    // one.
    if (name.length() &&
        strncasecmp(name.c_str(), PRINTER_BLE_NAME_PREFIX,
                    strlen(PRINTER_BLE_NAME_PREFIX)) == 0 &&
        d.getRSSI() > bestRssi) {
      bestRssi = d.getRSSI();
      s_dev = d;
      found = true;
    }
  }
  scan->clearResults();
  if (!found) {
    if (verbose) setStatus("Printer not found");
    return false;
  }
  Serial.printf("[print] picked %s (rssi=%d)\n",
                s_dev.getAddress().toString().c_str(), bestRssi);
  s_dev_valid = true;

  if (verbose) setStatus("Connecting to printer...");
  if (!bleConnectAttempts(PRINTER_CONNECT_ATTEMPTS)) {
    if (verbose) setStatus("Printer connect failed");
    return false;
  }
  if (!findWriteChar()) {
    if (verbose) setStatus("Printer: no writable channel");
    s_cli->disconnect();
    return false;
  }
  return true;
}

// Connect if not already connected. The AP is paused only for the attempt:
// connection ESTABLISHMENT reliably loses the radio to softAP beacons, but
// an established link survives with WiFi up (acknowledged writes can't lose
// data, at worst they slow down).
static bool bleEnsure(bool verbose) {
  if (printerLinked()) return true;
  s_chr = nullptr;
  webServerPauseAP();
  bool ok = bleConnect(verbose);
  webServerResumeAP();
  return ok;
}

static bool bleWrite(const uint8_t* d, size_t n) {
  uint16_t mtu = s_cli->getMTU();
  size_t chunk = (mtu > 23) ? (size_t)(mtu - 3) : 20;
  // Ack'd writes: printer-paced, larger chunks fine. Unack'd fallback: crawl.
  size_t cap = s_ackWrites ? 128 : 32;
  if (chunk > cap) chunk = cap;
  while (n) {
    size_t k = n < chunk ? n : chunk;
    // A failed write can also mean full buffers while the thermal head
    // catches up — back off and retry, and only give up if the link really
    // dropped (or the printer stalled for many seconds, e.g. out of paper).
    int tries = 0;
    while (!s_chr->writeValue(d, k, s_ackWrites)) {
      if (!s_cli->isConnected() || ++tries > 200) return false;
      vTaskDelay(pdMS_TO_TICKS(25));
    }
    d += k;
    n -= k;
    vTaskDelay(pdMS_TO_TICKS(s_ackWrites ? 2 : 25));
  }
  return true;
}

static void bleDisconnect() {
  if (s_cli && s_cli->isConnected()) s_cli->disconnect();
  s_chr = nullptr;
}

// ---------------------------------------------------------------------------
// Image pipeline
// ---------------------------------------------------------------------------
static int jpegCb(JPEGDRAW* p) {
  for (int y = 0; y < p->iHeight; ++y) {
    int dy = p->y + y;
    if (dy < 0 || dy >= kRgbH) continue;
    int w = p->iWidth;
    if (p->x + w > kRgbW) w = kRgbW - p->x;
    if (w <= 0) continue;
    memcpy(&s_rgb[dy * kRgbW + p->x], &p->pPixels[y * p->iWidth],
           w * sizeof(uint16_t));
  }
  return 1;
}

// Load /<session>/<n>.jpg into s_jpg. Returns byte count, 0 on failure.
static size_t loadPhoto(int n) {
  char path[32];
  snprintf(path, sizeof(path), "/%s/%d.jpg", s_session, n);
  storageLock();
  File f = SD.open(path, FILE_READ);
  size_t len = 0;
  if (f) {
    len = f.read(s_jpg, FRAME_MAX_PAYLOAD);
    f.close();
  }
  storageUnlock();
  return len;
}

// Decode s_jpg at half scale into s_rgb; sets s_dec_w/h. False on failure.
static bool decodePhoto(size_t len) {
  if (!s_jpeg.openRAM(s_jpg, len, jpegCb)) return false;
  s_jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
  s_dec_w = s_jpeg.getWidth() / 2;
  s_dec_h = s_jpeg.getHeight() / 2;
  if (s_dec_w > kRgbW || s_dec_h > kRgbH || s_dec_w < kW) {
    s_jpeg.close();
    return false;
  }
  int ok = s_jpeg.decode(0, 0, JPEG_SCALE_HALF);
  s_jpeg.close();
  return ok != 0;
}

// Crop to the WYSIWYG band (what the booth screen showed), rotate 90° so the
// photo's height runs across the 384-dot head, scale into s_gray. Applies
// gamma (thermal prints crush midtones) and keeps blank side margins so the
// edge-referenced paper doesn't clip the image.
static void buildGray() {
  static uint8_t gammaLut[256];
  static bool lutReady = false;
  if (!lutReady) {
    for (int i = 0; i < 256; ++i)
      gammaLut[i] = (uint8_t)(255.0f * powf(i / 255.0f, PRINTER_GAMMA) + 0.5f);
    lutReady = true;
  }

  const int mL = PRINTER_MARGIN_L, mR = PRINTER_MARGIN_R;
  const int cw = kW - mL - mR;  // content width across the head

  int bandH = (s_dec_w * 480) / 800;  // band height in decoded pixels
  if (bandH > s_dec_h) bandH = s_dec_h;
  int bandY0 = (s_dec_h - bandH) / 2;

  for (int y2 = 0; y2 < kL; ++y2) {
    int px = (y2 * s_dec_w) / kL;  // along the photo's width
    for (int x2 = 0; x2 < kW; ++x2) {
      if (x2 < mL || x2 >= kW - mR) {
        s_gray[y2 * kW + x2] = 255;  // margin stays white
        continue;
      }
      int xi = x2 - mL;
      int py = bandY0 + ((cw - 1 - xi) * bandH) / cw;  // down the band
      uint16_t c = s_rgb[py * kRgbW + px];
      int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
      r = (r << 3) | (r >> 2);
      g = (g << 2) | (g >> 4);
      b = (b << 3) | (b >> 2);
      s_gray[y2 * kW + x2] = gammaLut[(r * 77 + g * 150 + b * 29) >> 8];
    }
  }
}

// Floyd-Steinberg dither s_gray in place to 0/255.
static void dither() {
  static int16_t err[2][kW + 2];
  memset(err, 0, sizeof(err));
  for (int y = 0; y < kL; ++y) {
    int16_t* cur = err[y & 1];
    int16_t* nxt = err[(y + 1) & 1];
    memset(nxt, 0, sizeof(err[0]));
    for (int x = 0; x < kW; ++x) {
      int v = s_gray[y * kW + x] + cur[x + 1];
      int out = (v < 128) ? 0 : 255;
      int e = v - out;
      s_gray[y * kW + x] = (uint8_t)out;
      cur[x + 2] += (e * 7) >> 4;
      nxt[x] += (e * 3) >> 4;
      nxt[x + 1] += (e * 5) >> 4;
      nxt[x + 2] += (e * 1) >> 4;
    }
  }
}

// Send s_gray to the printer as one TSPL BITMAP. This unit runs the TSPL
// label language (confirmed by probe — it ignores ESC/POS entirely).
// NB: TSPL bitmap bits are inverted vs ESC/POS: 0 = printed (black) dot.
static uint8_t* s_bitmap = nullptr;  // kBytesPerLine * kL, PSRAM

static bool sendRaster() {
  if (!s_bitmap) s_bitmap = (uint8_t*)ps_malloc((size_t)kBytesPerLine * kL);
  if (!s_bitmap) return false;

  // 203 dpi = 8 dots/mm, so dots/8 = size in mm. GAP must carry units, and
  // 0 mm = continuous (receipt) paper — a malformed GAP makes PRINT hunt for
  // a label gap that never comes and the job dies silently.
  char hdr[160];
  int hl = snprintf(hdr, sizeof(hdr),
                    "\r\nSIZE %d mm,%d mm\r\nGAP 0 mm,0 mm\r\nDENSITY %d\r\n"
                    "DIRECTION 0\r\nCLS\r\n",
                    kW / 8, kL / 8, PRINTER_DENSITY);
  if (!bleWrite((const uint8_t*)hdr, hl)) return false;

  // Send the image as strips of 80 lines: the TSPL parser reads an exact
  // byte count per BITMAP, so a single lost byte only garbles one strip
  // instead of swallowing the PRINT command after a 30 KB block.
  for (int y0 = 0; y0 < kL; y0 += kStripLines) {
    int lines = (kL - y0 < kStripLines) ? (kL - y0) : kStripLines;
    char bh[48];
    int bl = snprintf(bh, sizeof(bh), "BITMAP 0,%d,%d,%d,0,", y0,
                      kBytesPerLine, lines);
    if (!bleWrite((const uint8_t*)bh, bl)) return false;

    for (int y = 0; y < lines; ++y) {
      const uint8_t* row = &s_gray[(y0 + y) * kW];
      uint8_t* out = &s_bitmap[y * kBytesPerLine];
      for (int xb = 0; xb < kBytesPerLine; ++xb) {
        uint8_t bits = 0xFF;  // all white
        for (int b = 0; b < 8; ++b)
          if (row[xb * 8 + b] == 0) bits &= ~(0x80 >> b);  // dark pixel = 0
        out[xb] = bits;
      }
    }
    if (!bleWrite(s_bitmap, (size_t)kBytesPerLine * lines)) return false;
    static const char kCrlf[] = "\r\n";
    if (!bleWrite((const uint8_t*)kCrlf, 2)) return false;
  }

  static const char kPrint[] = "PRINT 1\r\n";
  return bleWrite((const uint8_t*)kPrint, sizeof(kPrint) - 1);
}

// ---------------------------------------------------------------------------
// The job
// ---------------------------------------------------------------------------
static bool ensureBuffers() {
  if (!s_jpg) s_jpg = (uint8_t*)ps_malloc(FRAME_MAX_PAYLOAD);
  if (!s_rgb) s_rgb = (uint16_t*)ps_malloc(kRgbW * kRgbH * sizeof(uint16_t));
  if (!s_gray) s_gray = (uint8_t*)ps_malloc((size_t)kW * kL);
  return s_jpg && s_rgb && s_gray;
}

// Bring-up: set to 1 to replace the photo job with a 4-step probe that
// pinpoints where this unit's TSPL parser gives up. Which of A/B/C/D appear
// on paper tells the story:
//   A only   -> the GAP/DENSITY/DIRECTION header lines poison the job
//   A,B only -> raw-binary BITMAP data is the problem (any size)
//   A,B,C    -> full-size strips/volume are the problem (pacing)
//   all four -> everything works; the photo path itself has the bug
#define PRINTER_DIAG 0

static bool sendStr(const char* s) {
  return bleWrite((const uint8_t*)s, strlen(s));
}

static void doDiag() {
  // Round 1 (A-D) proved: headers OK, raw-binary BITMAP OK, one full-width
  // strip OK. Round 2 isolates what the real photo job adds: label length,
  // multiple strips per job, and data volume.
  setStatus("Printing test E/F...");
  if (!s_bitmap && !(s_bitmap = (uint8_t*)ps_malloc((size_t)kBytesPerLine * kL)))
    return;
  for (int y = 0; y < 80; ++y)  // reusable stripe pattern
    memset(&s_bitmap[y * kBytesPerLine], ((y / 8) & 1) ? 0x00 : 0xFF,
           kBytesPerLine);

  // E: full 80mm label, two strips at opposite ends + marker text.
  sendStr("SIZE 48 mm,80 mm\r\nGAP 0 mm,0 mm\r\nCLS\r\nBITMAP 0,0,48,80,0,");
  bleWrite(s_bitmap, (size_t)kBytesPerLine * 80);
  sendStr("\r\nBITMAP 0,560,48,80,0,");
  bleWrite(s_bitmap, (size_t)kBytesPerLine * 80);
  sendStr("\r\nTEXT 100,300,\"3\",0,1,1,\"TEST E\"\r\nPRINT 1\r\n");
  vTaskDelay(pdMS_TO_TICKS(4000));

  // F: exact photo-job shape at HALF length: 40mm, 4 back-to-back strips.
  sendStr("\r\nSIZE 48 mm,40 mm\r\nGAP 0 mm,0 mm\r\nDENSITY 12\r\n"
          "DIRECTION 0\r\nCLS\r\n");
  for (int y0 = 0; y0 < 320; y0 += 80) {
    char bh[48];
    int bl = snprintf(bh, sizeof(bh), "BITMAP 0,%d,%d,80,0,", y0,
                      kBytesPerLine);
    bleWrite((const uint8_t*)bh, bl);
    bleWrite(s_bitmap, (size_t)kBytesPerLine * 80);
    sendStr("\r\n");
  }
  sendStr("TEXT 100,140,\"3\",0,1,1,\"TEST F\"\r\nPRINT 1\r\n");
  vTaskDelay(pdMS_TO_TICKS(4000));
  setStatus("Check paper: E? F?");
}

static void doJobLinked();

static void doJob() {
  if (!ensureBuffers()) {
    setStatus("Print: out of memory");
    return;
  }
  if (!bleEnsure(true)) return;  // status already set
  doJobLinked();
}

static void doJobLinked() {
#if PRINTER_DIAG
  doDiag();
  return;
#endif

  int printed = 0;
  for (int i = 1; i <= s_count; ++i) {
    setStatus("Printing %d/%d...", i, (int)s_count);
    size_t len = loadPhoto(i);
    if (len == 0 || !decodePhoto(len)) {
      Serial.printf("[print] photo %d load/decode failed\n", i);
      continue;
    }
    buildGray();
    dither();
    if (!sendRaster()) {
      setStatus("Print failed (link lost)");
      bleDisconnect();
      return;
    }
    // Give PRINT 1 time to hit paper before streaming the next photo.
    vTaskDelay(pdMS_TO_TICKS(800));
    printed++;
  }
  // Deliberately NOT disconnecting: the link stays warm so the next guest's
  // print starts instantly. The task watchdog reconnects if it ever drops.
  setStatus(printed ? "Print done - tear off!" : "Nothing to print");
}

static void printTask(void*) {
  // Connect right after boot (quietly) so the first print starts instantly,
  // then keep the link alive with a backoff watchdog.
  vTaskDelay(pdMS_TO_TICKS(2500));  // let the display/AP/session settle first
  uint32_t backoff = 5000;
  uint32_t next_try = millis();
  for (;;) {
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) > 0) {
      s_busy = true;
      doJob();
      s_busy = false;
      // Let the result message sit for a bit, then clear (unless a new job
      // started meanwhile).
      vTaskDelay(pdMS_TO_TICKS(5000));
      if (!s_busy) s_status[0] = '\0';
      continue;
    }
    // Idle tick: (re)connect quietly if the link is down, with backoff so a
    // powered-off printer doesn't keep yanking the WiFi AP for scans.
    if (!printerLinked() && (int32_t)(millis() - next_try) >= 0) {
      if (bleEnsure(false)) {
        backoff = 5000;
        Serial.println("[print] link up (standby)");
      } else {
        backoff = backoff < 120000 ? backoff * 2 : 120000;
      }
      next_try = millis() + backoff;
    }
  }
}

// ---------------------------------------------------------------------------
void printerBegin() {
  NimBLEDevice::init("");
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // max TX power; coex eats margin
  // Core 0, below the UART receiver's priority — printing must never starve
  // the preview link.
  xTaskCreatePinnedToCore(printTask, "print", 8192, nullptr, 1, &s_task, 0);
  Serial.println("[print] BLE ready");
}

bool printerBusy() { return s_busy; }

const char* printerStatus() { return s_status; }

bool printerStartJob(const char* sessionId, int photoCount) {
  if (s_busy || !s_task || photoCount <= 0) return false;
  strncpy(s_session, sessionId, SESSION_ID_LEN);
  s_session[SESSION_ID_LEN] = '\0';
  s_count = photoCount;
  s_busy = true;  // set before notify so the UI sees it immediately
  xTaskNotifyGive(s_task);
  return true;
}
