# ESP32 Photo Booth — firmware

Two-board hackathon photo booth, implemented from [photobooth_build_plan.md](photobooth_build_plan.md).

- **camera-node/** — XIAO ESP32-S3 Sense (OV2640). Streams a QVGA JPEG preview
  over UART; grabs a full-res still on command.
- **main-node/** — VIEWE 4.3″ board (LVGL touchscreen + SD). UI, session state
  machine, JPEG decode + display, SD storage, WiFi softAP + HTTP gallery + QR.
- **shared/photobooth/** — the UART wire protocol (framing + CRC-32) and the
  shared config. Included by **both** projects (`lib_extra_dirs = ../shared`),
  so the two firmwares can never drift on the contract.

```
PhotoBooth/
├── shared/photobooth/      photobooth_config.h · photobooth_protocol.{h,cpp}
├── camera-node/            platformio.ini · src/ (main.cpp, camera_pins.h)
└── main-node/              platformio.ini · include/lv_conf.h · src/
                            main · uart_link · storage · qr_code · web_server
                            · display_bsp · ui · session
```

## Build

Each side is its own PlatformIO project — open the folder and build/upload:

```sh
cd camera-node && pio run -t upload      # XIAO
cd main-node   && pio run -t upload      # VIEWE
```

## ⚠️ Before it will run — confirm these (build plan §2)

These are marked `TODO` / placeholder in the source and **must** be set to your
actual hardware before anything works:

1. **VIEWE UART pins** — `VIEWE_UART_RX_PIN` / `VIEWE_UART_TX_PIN` in
   [shared/photobooth/photobooth_config.h](shared/photobooth/photobooth_config.h).
   Pick two genuinely-free GPIOs off the VIEWE header.
2. **VIEWE panel + touch** — every pin and RGB timing in
   [main-node/src/display_bsp.cpp](main-node/src/display_bsp.cpp) is a
   placeholder. Copy the real values from the VIEWE 4.3″ vendor demo (RGB data
   pins, DE/VSYNC/HSYNC/PCLK, backlight, GT911 touch I2C). The
   `Arduino_ESP32RGBPanel` constructor arguments must also match your installed
   Arduino_GFX version.
3. **SD pins** — [main-node/src/storage.cpp](main-node/src/storage.cpp) assumes
   default `SD_MMC` 1-bit pins; call `SD_MMC.setPins(...)` if your board differs.

Wiring (crossover + shared ground) is in build plan §2. XIAO camera pins are
fixed and already set in
[camera-node/src/camera_pins.h](camera-node/src/camera_pins.h).

## Build order (test each milestone before the next — build plan §10)

1. **UART link** — the framing/CRC in `shared/` is the foundation. Watch
   `uartFramesOk()` / `uartFramesBadCrc()` on the VIEWE.
2. **Preview stream** — `PREVIEW_FRAME` → decode → screen. Measure fps; lower
   `PREVIEW_FRAMESIZE`/quality if poor (build plan §11).
3. **UI + selector + button** — idle screen, `sessionLoop()` skeleton.
4. **Countdown + flash** — visual only.
5. **Full-res capture** — `CMD_CAPTURE` / `CAPTURE_FRAME`, the preview↔still
   switch on the XIAO.
6. **SD save** — `/<sessionID>/<n>.jpg`; verify on a computer.
7. **Multi-shot loop** — inter-shot wait for 2 and 4 shots.
8. **WiFi + server + QR** — softAP, gallery, JPEGs, QR; phone download.
9. **Polish + filters** — overlays, timeouts, error handling.

## The UART contract (build plan §4)

`0xA5 0x5A | TYPE | LENGTH(u32 LE) | PAYLOAD | CRC32(LE)`, CRC-32 over
`TYPE‖LENGTH‖PAYLOAD`. Implemented once in
[shared/photobooth/photobooth_protocol.cpp](shared/photobooth/photobooth_protocol.cpp)
(`writeFrame`, `FrameParser`). Start at 2 Mbps; drop to 1 Mbps on corruption.

## Notes / v1 scope decisions

- **Gallery framing is server-side** — the "frame" is white padding + shadow in
  the served HTML/CSS ([web_server.cpp](main-node/src/web_server.cpp)), so the
  ESP32 never composites images (build plan §9).
- **Result-screen thumbnails** are deferred; the result screen shows the QR +
  photo count. Decoding N full-res JPEGs on-device is the heavy path we avoid;
  add later via an LVGL SD filesystem driver if wanted.
- **Escape hatch**: if UART preview is too slow and 4 pins are free on the
  VIEWE, switch the transport to SPI (build plan §11). The framing layer stays
  the same; only the byte pipe changes.
