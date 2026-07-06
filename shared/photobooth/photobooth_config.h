// =============================================================================
// photobooth_config.h  —  Shared configuration for BOTH firmware projects.
//
// This is the single source of truth for the values in §13 of the build plan.
// Both camera-node (XIAO) and main-node (VIEWE) include this file so the two
// firmwares can never drift apart on the parts of the contract they share
// (baud rate, frame magic bytes, camera resolutions, session timing).
//
// Board-specific pins that only exist on one side are still kept here, grouped
// per board, so there is one place to look.
// =============================================================================
#pragma once

// ---------------------------------------------------------------------------
// UART link
// ---------------------------------------------------------------------------
// This is the preview-fps bottleneck: QVGA JPEG frames are ~8-15 KB, and at
// 8N1 the effective throughput is baud/10 bytes/s. 500000 baud (~50 KB/s)
// capped preview at ~4 fps; 2000000 (~200 KB/s) allows ~15-20 fps. The S3
// UART is good to 5 Mbps; the limit in practice is wiring — keep the link
// wires short with a solid common ground. If the bad-CRC counter on the
// diag bar climbs, step down: 2000000 -> 1500000 -> 1000000.
#define UART_BAUD 2000000

// ---- VIEWE (main node) UART pins --------------------------------------------
// !! OPEN ITEM (build plan §2 / §11) !!
// The VIEWE 4.3" RGB panel consumes most of the S3's GPIO. These TWO pins MUST
// be confirmed against the exact VIEWE model's exposed header before wiring —
// they are PLACEHOLDERS chosen so the code compiles, NOT verified free pins.
// Once two genuinely-free GPIOs are known, just set them here; on the S3 any
// free GPIO can be routed to a UART through the GPIO matrix.
//
// The VIEWE RX/TX header breaks out UART0 = GPIO43/44 — but BOTH of those
// through-hole pads are now dead (44 first, then 43). Remapped to the last
// two wireable pins on this board:
//   * RX = GPIO0 (IO0/BOOT) — receives the camera's preview/still data.
//     Wire XIAO TX (D6) here. NB: IO0 is a strapping pin, so the camera must
//     not be driving it low at the instant the VIEWE resets, or the VIEWE
//     boots into download mode. The keepalive gate covers this: the camera
//     starts silent and goes silent again within PREVIEW_STALE_MS whenever
//     the display stops asking. Residual risk is a warm VIEWE reset mid-
//     stream; a power-cycle recovers it.
//   * TX = GPIO17 — the display->camera command channel. Wire it to XIAO
//     RX (D7). This pad previously carried the link and is known good.
#define VIEWE_UART_RX_PIN 0
#define VIEWE_UART_TX_PIN 17

// ---- VIEWE (main node) SD card pins -----------------------------------------
// The UEDX80480043E wires the TF slot to a plain SPI bus, NOT the SDMMC
// peripheral (see VIEWE's examples/esp_idf/sd_card_spi for this model). These
// are the S3's native FSPI pins and are free of the RGB panel / touch / UART.
#define VIEWE_SD_CS 10
#define VIEWE_SD_MOSI 11
#define VIEWE_SD_CLK 12
#define VIEWE_SD_MISO 13

// ---- XIAO (camera node) UART pins -------------------------------------------
// Fixed by the XIAO ESP32-S3 silkscreen: D6 = GPIO43 (TX), D7 = GPIO44 (RX).
#define XIAO_UART_TX_PIN 43  // D6
#define XIAO_UART_RX_PIN 44  // D7

// ---------------------------------------------------------------------------
// Protocol framing (see photobooth_protocol.h for the full frame layout)
// ---------------------------------------------------------------------------
#define FRAME_MAGIC0 0xA5
#define FRAME_MAGIC1 0x5A

// Largest payload the receiver will accept. A corrupt LENGTH field could
// otherwise ask us to allocate gigabytes; this caps it. Full-res SVGA JPEG is
// typically 30-80 KB, so 256 KB is a comfortable ceiling.
#define FRAME_MAX_PAYLOAD (256u * 1024u)

// ---------------------------------------------------------------------------
// Camera (XIAO side)
// ---------------------------------------------------------------------------
// Preview: smallest acceptable frame — this directly sets preview fps.
// Preview is deliberately low quality: q14 QVGA is ~5-9 KB per frame, which
// keeps the live view at 20+ fps over the 2 Mbaud link. The saved photo never
// goes through this path, so preview compression costs nothing in the result.
#define PREVIEW_FRAMESIZE FRAMESIZE_QVGA  // 320x240; drop to QQVGA if fps poor
#define PREVIEW_JPEG_QUALITY 14           // higher number = smaller/faster

// Full-res still: use the OV2640's native 2 MP. UXGA q10 JPEGs run
// ~100-200 KB — about 1 s to transfer at 2 Mbaud, and well under
// FRAME_MAX_PAYLOAD.
#define STILL_FRAMESIZE FRAMESIZE_UXGA  // 1600x1200 (sensor native)
#define STILL_JPEG_QUALITY 10           // lower number = better quality

// ---- Preview keepalive -------------------------------------------------------
// The camera streams preview only while the display keeps asking for it: the
// display repeats CMD_START_PREVIEW every PREVIEW_KEEPALIVE_MS, and the camera
// stops transmitting if it hasn't heard any command for PREVIEW_STALE_MS.
// Critical for flashing: the XIAO's TX shares the GPIO43 net with the VIEWE's
// USB flashing bridge, so a camera that streams unconditionally corrupts every
// esptool upload to the main node.
#define PREVIEW_KEEPALIVE_MS 1000
#define PREVIEW_STALE_MS 3000

// ---------------------------------------------------------------------------
// Session timing (main node). Seconds unless the name says _MS.
// ---------------------------------------------------------------------------
#define COUNTDOWN_START 4      // every shot counts down 4..3..2..1
#define COUNTDOWN_SHORT 4      // subsequent shots use the same countdown
#define INTERSHOT_SECONDS 3    // "get ready" gap between shots
#define FLASH_MS 200           // white flash duration at capture
#define CAPTURE_TIMEOUT_MS 3000  // wait for CAPTURE_FRAME before retrying
#define CAPTURE_RETRIES 3      // give up (and abort session) after this many
#define RESULT_TIMEOUT_S 60    // result screen auto-returns to IDLE after this

// Allowed shot-count choices offered on the idle screen.
#define SHOT_COUNTS \
  { 1, 2, 4 }

// ---------------------------------------------------------------------------
// Thermal printer (Xprinter P203A over BLE, main node)
// ---------------------------------------------------------------------------
// The printer is found by scanning for a BLE device whose advertised name
// starts with this prefix. Use the FULL unit name: plain "Printer001" also
// matched a neighboring Xprinter and the booth kept connecting to the wrong
// (out-of-range) one. Among multiple matches the strongest signal wins.
#define PRINTER_BLE_NAME_PREFIX "Printer001-77F3"
#define PRINTER_DOTS 384       // 58mm head at 203dpi = 384 dots per line
#define PRINTER_PRINT_LEN 640  // dots along the paper per photo (~80mm)
// Connection setup flakes under WiFi coexistence — observed needing 1..6
// tries. The link is held open permanently and reconnected by a watchdog.
#define PRINTER_CONNECT_ATTEMPTS 8

// Print tuning (calibrated on this unit):
#define PRINTER_DENSITY 8   // 0-15; 12 came out far too dark
#define PRINTER_GAMMA 0.5f // <1 brightens midtones before dithering
// Blank margins inside the bitmap (dots). The paper is edge-referenced in
// the holder, so the right side falls off the printable area first.
#define PRINTER_MARGIN_L 4
#define PRINTER_MARGIN_R 20

// ---------------------------------------------------------------------------
// WiFi / web server (main node)
// ---------------------------------------------------------------------------
#define AP_SSID "PhotoBooth"
#define AP_PASSWORD ""          // "" = open network; set a value to lock it
#define AP_IP "192.168.4.1"     // default softAP IP; the QR encodes this
#define HTTP_PORT 80

// ---------------------------------------------------------------------------
// Cloud gallery (optional — see cloud/README.md for the 5-minute setup)
// ---------------------------------------------------------------------------
// When the booth can reach the internet through this WiFi (your phone's
// hotspot works), every session uploads to the Cloudflare Worker and the QR
// becomes a normal https link guests open on their own mobile data — nobody
// joins the booth WiFi. Leave STA_SSID empty to disable; the booth then
// falls back to the local-AP gallery automatically.
#define STA_SSID "VankeYunCheng"
#define STA_PASSWORD "SJGS6666"
#define CLOUD_BASE_URL "https://photobooth.arontheb.workers.dev"  // no trailing /
#define CLOUD_UPLOAD_KEY "random_key_67"   // the same UPLOAD_KEY value
