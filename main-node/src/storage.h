// =============================================================================
// storage.h  —  SD card storage (build plan §6d).
//
// Photos are saved verbatim (no re-encode) as /<sessionID>/<n>.jpg.
// Uses the SPI SD driver: the UEDX80480043E wires the TF slot to SPI
// (CS/MOSI/CLK/MISO = GPIO10-13), not the SDMMC peripheral.
// =============================================================================
#pragma once

#include <Arduino.h>

// Mount the SD card. Returns false if the card is missing/unreadable.
bool storageBegin();

// SD access mutex. The print task reads the card from core 0 while the UI,
// session and web server use it from core 1 — FatFS is not reentrant, so
// EVERY direct SD.* access outside storage.cpp must hold this lock.
void storageLock();
void storageUnlock();

// Generate a fresh 4-char session id (e.g. "A1B2") into `out` (>= 5 bytes) and
// create its folder /<id>. Returns false on failure.
bool storageNewSession(char* out);

// Save one JPEG for the current session as /<sessionId>/<index>.jpg (1-based).
// Returns false on write failure.
bool storageSaveJpeg(const char* sessionId, int index, const uint8_t* data,
                     size_t len);

// Record which filter overlay was active for shot <index> as the sidecar
// /<sessionId>/<index>.flt (contains the PNG filename, e.g. "hearts.png").
// No-op when name is empty (no filter). The web gallery reads this to
// composite the overlay into the downloaded photo.
bool storageSaveFilterTag(const char* sessionId, int index, const char* name);
