// =============================================================================
// storage.cpp  —  see storage.h
// =============================================================================
#include "storage.h"

#include <SD.h>
#include <SPI.h>

#include "app.h"

static SemaphoreHandle_t s_sd_mtx = nullptr;

void storageLock() {
  if (s_sd_mtx) xSemaphoreTake(s_sd_mtx, portMAX_DELAY);
}

void storageUnlock() {
  if (s_sd_mtx) xSemaphoreGive(s_sd_mtx);
}

bool storageBegin() {
  s_sd_mtx = xSemaphoreCreateMutex();
  // The UEDX80480043E's TF slot hangs off SPI (GPIO10-13), NOT the SDMMC
  // peripheral — SD_MMC can never mount on this board. The pins are the S3's
  // native FSPI set, so a fast clock is safe on the short on-board traces.
  SPI.begin(VIEWE_SD_CLK, VIEWE_SD_MISO, VIEWE_SD_MOSI, VIEWE_SD_CS);
  if (!SD.begin(VIEWE_SD_CS, SPI, 25000000)) {
    Serial.println("[sd] mount failed");
    return false;
  }
  uint8_t type = SD.cardType();
  if (type == CARD_NONE) {
    Serial.println("[sd] no card");
    return false;
  }
  Serial.printf("[sd] mounted, %llu MB\n", SD.cardSize() / (1024 * 1024));
  return true;
}

bool storageNewSession(char* out) {
  // 4 chars from an unambiguous alphabet (no 0/O/1/I) so the URL is easy to
  // read off the screen if needed.
  static const char alpha[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  const int n = sizeof(alpha) - 1;
  for (int attempt = 0; attempt < 8; ++attempt) {
    for (int i = 0; i < SESSION_ID_LEN; ++i) out[i] = alpha[esp_random() % n];
    out[SESSION_ID_LEN] = '\0';

    char path[16];
    snprintf(path, sizeof(path), "/%s", out);
    storageLock();
    bool taken = SD.exists(path);
    bool made = !taken && SD.mkdir(path);
    storageUnlock();
    if (taken) continue;  // collision, try again
    if (made) return true;
  }
  Serial.println("[sd] could not create session folder");
  return false;
}

bool storageSaveJpeg(const char* sessionId, int index, const uint8_t* data,
                     size_t len) {
  char path[32];
  snprintf(path, sizeof(path), "/%s/%d.jpg", sessionId, index);
  storageLock();
  File f = SD.open(path, FILE_WRITE);
  if (!f) {
    storageUnlock();
    Serial.printf("[sd] open %s failed\n", path);
    return false;
  }
  size_t wrote = f.write(data, len);
  f.close();
  storageUnlock();
  if (wrote != len) {
    Serial.printf("[sd] short write %s (%u/%u)\n", path, (unsigned)wrote,
                  (unsigned)len);
    return false;
  }
  Serial.printf("[sd] saved %s (%u bytes)\n", path, (unsigned)len);
  return true;
}

bool storageSaveFilterTag(const char* sessionId, int index, const char* name) {
  if (!name || !name[0]) return true;  // no filter -> no sidecar
  char path[32];
  snprintf(path, sizeof(path), "/%s/%d.flt", sessionId, index);
  storageLock();
  File f = SD.open(path, FILE_WRITE);
  if (!f) {
    storageUnlock();
    Serial.printf("[sd] open %s failed\n", path);
    return false;
  }
  f.print(name);
  f.close();
  storageUnlock();
  return true;
}
