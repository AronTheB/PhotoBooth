# ESP32 Photo Booth — Build Plan

A hackathon photo booth built from two ESP32-S3 boards. This document is written to be handed to **Claude Code** as an implementation spec. It describes the hardware, the split of responsibilities between the two boards, the UART protocol between them, the firmware for each side, the on-screen photo-session flow, and the download/QR mechanism.

Read the "Build order" section first — implement in that order, testing each milestone before moving on. Don't try to build the whole thing at once; the live-preview-over-UART link is the riskiest part and should be proven before anything else is layered on.

---

## 1. Goal / user experience

1. The display shows a **live camera preview**.
2. On-screen the user picks how many shots they want: **1, 2, or 4**, then presses a **"Take photo"** button.
3. A **countdown from 5** appears over the preview.
4. At zero, the screen **flashes white** and a full-resolution photo is captured.
5. If more shots were requested, wait ~3 seconds (with a short countdown), then shoot again — repeat until all N shots are taken.
6. When the session is done:
   - all photos are **saved to the SD card** in the display board,
   - a **QR code** is shown that lets a phone **download the photos**.
7. (Later phase) On-screen **frames/filters/decorations** around the preview, and optionally baked into the downloaded image.

---

## 2. Hardware

| Role | Board | Responsibilities |
|------|-------|------------------|
| **Camera node** | XIAO ESP32-S3 Sense (OV2640) | Capture frames. Stream a low-res JPEG preview continuously; grab a full-res JPEG on command. |
| **UI / main node** | VIEWE 4.3″ board (ESP32-S3 + LVGL touchscreen + SD slot) | Everything else: UI, state machine, JPEG decode + display, SD storage, WiFi AP + web server + QR. |

The two boards are linked by **UART** (this is a deliberate, already-decided constraint — see §11 for why, and the escape hatch if preview is too slow).

### Wiring

UART is a crossover (each board's TX → the other's RX) plus a shared ground.

| XIAO ESP32-S3 Sense | → | VIEWE 4.3″ board |
|---------------------|---|------------------|
| `D6` / GPIO43 (TX) | → | VIEWE UART **RX** (a free GPIO) |
| `D7` / GPIO44 (RX) | ← | VIEWE UART **TX** (a free GPIO) |
| `GND` | ↔ | `GND` (mandatory — link floats without it) |
| `VBUS` (5V in) | ← | VIEWE `5V` / `VIN` (if powering XIAO from the main board) |

**Open item (must confirm):** the VIEWE 4.3″ RGB panel consumes most of the S3's GPIO. Two free GPIOs must be identified from the board's exposed header for the UART. Do **not** assume arbitrary pin numbers — confirm against the specific VIEWE model's pinout, then set them in config (§13). On the S3 any free GPIO can be routed to a UART via the GPIO matrix, so once two spare pins are known, assignment is purely in software.

Power: drive the VIEWE board from its USB-C and feed the XIAO from the VIEWE `5V` pin, **or** power each board from its own USB-C and connect **only** `GND↔GND` (never tie two USB 5V rails together).

---

## 3. System architecture

```
┌─────────────────────────┐        UART         ┌──────────────────────────────────┐
│  XIAO ESP32-S3 Sense     │  (framed packets)   │  VIEWE 4.3″ ESP32-S3 (main)      │
│                          │ ───── preview ────▶ │                                  │
│  • OV2640 camera         │ ───── full-res ───▶ │  • LVGL UI + touch               │
│  • preview JPEG stream   │ ◀──── commands ──── │  • JPEG decode → display         │
│  • full-res on command   │                     │  • session state machine         │
└─────────────────────────┘                     │  • SD card storage               │
                                                 │  • WiFi softAP + HTTP server     │
                                                 │  • QR code generation            │
                                                 └──────────────────────────────────┘
                                                             │  WiFi (softAP)
                                                             ▼
                                                    📱 phone scans QR,
                                                       downloads photos
```

**Single-camera constraint (important):** the OV2640 cannot stream preview *and* take a full-res still at the same time. The camera node must stop the preview stream, switch resolution, grab the full-res frame, send it, then resume preview. During that ~0.5–1.5 s the on-screen preview freezes — which is fine, because the screen is flashing white at that exact moment.

---

## 4. UART protocol

A raw UART byte stream has no message boundaries, so every message is **framed**. Both directions use the same frame format.

### Frame format

```
┌────────┬────────┬────────┬──────────────┬───────────┬──────────┐
│ 0xA5   │ 0x5A   │  TYPE  │  LENGTH (u32) │  PAYLOAD   │ CRC32    │
│ 1 byte │ 1 byte │ 1 byte │  4 bytes LE  │ LENGTH B   │ 4 bytes  │
└────────┴────────┴────────┴──────────────┴───────────┴──────────┘
```

- `0xA5 0x5A` — magic start bytes. Receiver hunts for these to resync if it loses framing.
- `TYPE` — message type (below).
- `LENGTH` — payload length in bytes, little-endian u32.
- `PAYLOAD` — TYPE-dependent.
- `CRC32` — CRC-32 over `TYPE || LENGTH || PAYLOAD`. Receiver drops the frame on mismatch (partial/corrupt frame) and resyncs on the next magic bytes.

### Message types

| TYPE | Name | Direction | Payload |
|------|------|-----------|---------|
| `0x01` | `PREVIEW_FRAME` | XIAO → VIEWE | JPEG bytes (low-res preview frame) |
| `0x02` | `CAPTURE_FRAME` | XIAO → VIEWE | JPEG bytes (full-res still) |
| `0x10` | `CMD_START_PREVIEW` | VIEWE → XIAO | none |
| `0x11` | `CMD_STOP_PREVIEW` | VIEWE → XIAO | none |
| `0x12` | `CMD_CAPTURE` | VIEWE → XIAO | 1 byte: resolution enum (optional) |
| `0x20` | `ACK` | XIAO → VIEWE | 1 byte: the TYPE being acked |
| `0x21` | `ERR` | XIAO → VIEWE | 1 byte error code + optional message |

### Baud rate

Start at **2,000,000 baud** (2 Mbps). The S3 UART handles this over short jumper wires. If you see corruption, drop to 1,000,000. This directly caps preview frame rate — see §11.

Keep wires short (<15 cm), twisted with ground if possible.

---

## 5. Camera node firmware (XIAO ESP32-S3 Sense)

**Platform:** PlatformIO, Arduino-ESP32, `esp32-camera` (the `esp_camera` component). Uses PSRAM.

### Behavior

- On boot: init OV2640. Configure camera with PSRAM frame buffers, JPEG output.
- Main loop is a small state machine: `PREVIEW` (default) vs `IDLE`.
- **Preview mode:** capture a small JPEG (see resolution below), wrap it as `PREVIEW_FRAME`, send over UART. Loop as fast as bandwidth allows. Drop frames rather than block — never let the UART TX buffer stall the capture loop.
- **On `CMD_CAPTURE`:**
  1. stop preview,
  2. reconfigure sensor to full-res (or use a second framebuffer config),
  3. capture one frame,
  4. send it as `CAPTURE_FRAME`,
  5. send `ACK`,
  6. resume preview.
- Respond to `CMD_STOP_PREVIEW` / `CMD_START_PREVIEW` accordingly.

### Camera settings

| Use | Resolution | JPEG quality | Notes |
|-----|-----------|--------------|-------|
| Preview | `FRAMESIZE_QVGA` (320×240) — drop to 240×240 or QQVGA if fps is poor | ~12 (lower = smaller/faster) | Target smallest acceptable frame. |
| Full-res still | `FRAMESIZE_SVGA` (800×600) or `XGA` (1024×768) | ~10 (higher quality) | Big enough for a nice download, small enough that transfer stays ~1 s. Configurable. |

Reconfiguring framesize on the OV2640 at runtime can require re-init of the sensor; if switching is unreliable, an acceptable fallback is to run the camera permanently at the still resolution and downscale for preview — but that makes preview frames bigger and slower, so prefer runtime switching first.

---

## 6. Main node firmware (VIEWE 4.3″ board)

**Platform:** PlatformIO, Arduino-ESP32. Libraries: **LVGL** (already the board's UI stack), a **JPEG decoder** (`JPEGDEC` or `TJpgDec`), **SD**/`SD_MMC`, **WiFi** + an HTTP server (`ESPAsyncWebServer` or the built-in `WebServer`), and a **QR code generator** (`QRCode` by ricmoo / `qrcodegen`).

This node runs several concerns concurrently. Structure them so the LVGL tick/handler is never starved:

### 6a. UART receiver

- Runs continuously (ideally on a dedicated task on core 0, with LVGL/UI on core 1).
- Parses frames (§4). Maintains a resync state machine on the magic bytes.
- On `PREVIEW_FRAME`: hand the JPEG to the decoder, render into the preview image object (see 6c). If a new frame arrives while the previous is still decoding, drop the old one — always show the freshest.
- On `CAPTURE_FRAME`: hold the JPEG buffer for the session (save to SD, §6d).

### 6b. LVGL UI — screens

- **Preview screen (idle):** full-screen live preview image, a shot-count selector (1 / 2 / 4), and a big "Take photo" button. (Later: decorative frame overlay.)
- **Countdown overlay:** large number (5→1) centered over the preview.
- **Flash overlay:** a full-screen white object toggled on for ~150–250 ms at capture. Optionally also push the backlight to max for the flash.
- **Result screen:** the QR code, thumbnails of the session's photos, and a "Done / start over" button that returns to idle.

### 6c. JPEG decode + preview rendering

- Decode each preview JPEG into a buffer sized to the display region.
- Use an LVGL `lv_canvas` or an `lv_img` fed from a decoded RGB565 buffer in PSRAM.
- Keep the decode target small (match preview resolution, scale up in the display). Decoding at video rate is CPU-heavy — this is the main reason preview resolution stays low.

### 6d. SD card storage

- On session start, generate a short **session ID** (e.g. 4 chars, `A1B2`).
- Save each capture as `/<sessionID>/<n>.jpg` (e.g. `/A1B2/1.jpg`).
- Write directly from the received `CAPTURE_FRAME` JPEG bytes — no re-encoding needed.

### 6e. WiFi + web server + QR

- Bring up a **WiFi softAP** (e.g. SSID `PhotoBooth`, open or with a simple known password). Default AP IP is `192.168.4.1`.
- Run an HTTP server that serves files from SD:
  - `GET /s/<sessionID>` → a small generated HTML **gallery page** listing that session's photos (with a download-all link). Doing the "layout / frame" in served HTML/CSS keeps image compositing off the ESP32 entirely.
  - `GET /s/<sessionID>/<n>.jpg` → the raw photo from SD.
- The **QR code** encodes `http://192.168.4.1/s/<sessionID>`. Render the QR into an LVGL canvas on the result screen.
- Flow for the user: connect phone to the `PhotoBooth` WiFi → scan QR → browser opens the gallery → download.

> WiFi is only needed at the result stage. It coexists fine with UART, but to keep the preview loop light you can leave the AP up the whole time and simply not serve heavy traffic until a session finishes.

---

## 7. Session state machine (main node)

States and transitions. Timings are constants (§13).

```
IDLE ──(user picks 1/2/4 + presses Take photo)──▶ COUNTDOWN(first)
COUNTDOWN ──(reaches 0)──▶ CAPTURE
CAPTURE   ──(flash + CMD_CAPTURE + receive CAPTURE_FRAME + save)──▶ decide
decide    ── more shots? yes ──▶ INTERSHOT_WAIT ──▶ COUNTDOWN(short)
          ── more shots? no  ──▶ PROCESS
PROCESS   ──(finalize session, gen gallery + QR)──▶ RESULT
RESULT    ──(Done / timeout)──▶ IDLE
```

Detail:

- **IDLE:** preview streaming, selector + button live. Store chosen `shotCount`.
- **COUNTDOWN(first):** show 5→1, one number per second (`COUNTDOWN_START = 5`). Preview keeps running underneath.
- **CAPTURE:**
  1. show white **flash** overlay,
  2. send `CMD_CAPTURE` to XIAO,
  3. await `CAPTURE_FRAME` (with a timeout + retry),
  4. save JPEG to SD as `/<sessionID>/<shotIndex>.jpg`,
  5. hide flash, preview resumes automatically as XIAO restarts its stream.
- **INTERSHOT_WAIT:** if `shotIndex < shotCount`, wait `INTERSHOT_SECONDS = 3` (show a "get ready" + short countdown), then back to COUNTDOWN with `COUNTDOWN_SHORT = 3`.
- **PROCESS:** all shots captured. Finalize the session folder, build the gallery HTML, generate the QR bitmap.
- **RESULT:** show QR + thumbnails. "Done" (or a `RESULT_TIMEOUT`) returns to IDLE and clears session state.

Edge cases to handle:
- `CAPTURE_FRAME` timeout → retry `CMD_CAPTURE` up to N times, then show an error and abort the session gracefully.
- SD write failure → surface an on-screen error rather than hanging.
- User walks away mid-session → `RESULT_TIMEOUT` / idle timeout resets to IDLE.

---

## 8. Download / QR design (why local softAP)

The QR must point somewhere a phone can actually reach. The self-contained, no-internet approach is: the **main board hosts the photos on its own WiFi AP** and the QR encodes a LAN URL. This needs no backend and works anywhere (crucial at a venue with no reliable WiFi).

- Pro: fully offline, deterministic URL, simple.
- Con: the phone has to join the booth's WiFi first (briefly leaving internet). Put a short "Connect to WiFi 'PhotoBooth', then scan" instruction on the result screen.

A cloud-upload variant (photo → internet host → QR of a public URL) is possible later but needs internet + a backend and is out of scope for the first version.

---

## 9. Filters / frames (later phase — leave hooks)

Deferred, but design so it's cheap to add:

- **On-screen decoration (cosmetic):** an LVGL overlay image (PNG with transparency) on top of the preview and countdown. Purely visual, trivial to add. Provide a slot in the preview screen for an overlay `lv_img`.
- **Frames baked into the download:** easiest done **server-side** — the gallery HTML/CSS frames the photos when viewed/downloaded, so the ESP32 never composites images. Keep this option open by templating the gallery HTML.
- **On-device compositing** (a real printed/downloaded "strip"): heaviest option — decode N JPEGs, arrange on a PSRAM canvas, re-encode. Only pursue if a single composited file is required on-device. Not recommended for v1.

---

## 10. Build order (milestones — implement and test in sequence)

1. **UART link proof.** XIAO sends a counter/heartbeat framed packet; VIEWE parses frames and logs them. Confirm framing + CRC + resync work. *(De-risks the whole project.)*
2. **Preview stream.** XIAO streams QVGA JPEG; VIEWE decodes and shows it on the LVGL screen. Measure the achieved **fps** — this decides whether the transport is good enough (§11).
3. **UI + selector + button.** Add the 1/2/4 selector and Take-photo button; wire up the state machine skeleton (no capture yet).
4. **Countdown + flash.** Implement the countdown overlay and white flash purely visually.
5. **Full-res capture.** Implement `CMD_CAPTURE` / `CAPTURE_FRAME`, the preview↔still switch on the XIAO, and receive the still on the VIEWE.
6. **SD save.** Save captures to `/<sessionID>/<n>.jpg`; verify files on a computer.
7. **Multi-shot loop.** Wire the inter-shot wait and repeat for 2 and 4 shots.
8. **WiFi + web server + QR.** softAP, serve gallery + JPEGs, generate + show the QR. End-to-end phone download.
9. **Polish + (optional) filters.** Overlays, timeouts, error handling.

---

## 11. Risks & key decisions

- **UART bandwidth is the main risk.** At 2 Mbps the ceiling is ~200 KB/s after overhead. A ~8–12 KB QVGA JPEG → very roughly 10–18 preview fps *before* decode cost; the real limit may be JPEG decode on the VIEWE side. Mitigations, in order: lower preview resolution/quality, raise baud, drop stale frames aggressively. **Escape hatch:** if smooth preview proves impossible over UART, the higher-bandwidth alternative is **SPI** between the two boards (tens of Mbps) — but it needs 4 free pins on the VIEWE board and an SPI-slave receive implementation, so only switch if UART preview is genuinely too slow *and* the pins are available. (This project deliberately starts on UART for the 2-pin simplicity.)
- **VIEWE free-GPIO confirmation** (§2) blocks wiring — resolve first.
- **PSRAM required** on both boards (camera framebuffers on the XIAO; decode + display buffers on the VIEWE). Enable it in the PlatformIO build flags.
- **Dual-core scheduling** on the VIEWE: keep the UART receive/decode off the LVGL core so the UI stays responsive.
- **WiFi + RGB panel coexistence:** generally fine; if the panel glitches when the radio is active, only enable the AP at the result stage.
- **Single camera** can't preview + capture simultaneously (§3) — the preview freeze during capture is expected and hidden by the flash.

---

## 12. Suggested libraries

Confirm current versions when Claude Code sets up `platformio.ini`.

- **LVGL** — UI (already the board's stack; match the version the VIEWE board's demo uses).
- **esp32-camera** (`esp_camera`) — OV2640 on the XIAO.
- **JPEGDEC** (Larry Bank) or **TJpgDec** — JPEG decode on the VIEWE.
- **SD** / **SD_MMC** — SD card on the VIEWE.
- **ESPAsyncWebServer** (+ **AsyncTCP**) or the built-in **WebServer** — HTTP serving.
- **QRCode** (ricmoo) or **qrcodegen** — QR bitmap generation.

---

## 13. Config values to set (single header, both projects share the relevant ones)

```cpp
// ---- UART link ----
#define UART_BAUD              2000000
#define VIEWE_UART_RX_PIN      /* TODO: free GPIO on VIEWE */
#define VIEWE_UART_TX_PIN      /* TODO: free GPIO on VIEWE */
// XIAO side: TX = D6/GPIO43, RX = D7/GPIO44

// ---- Protocol ----
#define FRAME_MAGIC0           0xA5
#define FRAME_MAGIC1           0x5A

// ---- Camera ----
#define PREVIEW_FRAMESIZE      FRAMESIZE_QVGA   // 320x240
#define PREVIEW_JPEG_QUALITY   12
#define STILL_FRAMESIZE        FRAMESIZE_SVGA   // 800x600
#define STILL_JPEG_QUALITY     10

// ---- Session timing (seconds) ----
#define COUNTDOWN_START        5
#define COUNTDOWN_SHORT        3
#define INTERSHOT_SECONDS      3
#define FLASH_MS               200
#define CAPTURE_TIMEOUT_MS     3000
#define CAPTURE_RETRIES        3
#define RESULT_TIMEOUT_S       60

// ---- WiFi / server ----
#define AP_SSID                "PhotoBooth"
#define AP_PASSWORD            ""               // open, or set one
#define AP_IP                  "192.168.4.1"
#define SHOT_COUNTS            {1, 2, 4}
```

---

## 14. Notes for the implementer

- Prefer clear, commented, understandable code over clever code — the point is to be maintainable and explainable, not vibe-coded.
- Build strictly in the §10 order; do not proceed past a milestone until it's demonstrably working.
- Treat the UART protocol (§4) as the contract between the two firmware projects — implement the framing/CRC once, cleanly, and share the same constants on both sides.
- Keep the two firmware projects separate (two PlatformIO projects / two `main` files): `camera-node/` (XIAO) and `main-node/` (VIEWE).
