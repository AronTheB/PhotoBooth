// =============================================================================
// filters.cpp  —  see filters.h
// =============================================================================
#include "filters.h"

#include <PNGdec.h>
#include <SD.h>

#include "app.h"
#include "storage.h"

#define FILTERS_DIR "/filters"
static const int kMaxFilters = 16;

// Basename cap: must fit the .flt sidecars and the /f/<name> gallery URLs.
static const size_t kMaxFileName = 31;

static char g_paths[kMaxFilters][48];  // full SD path of each PNG
static char g_names[kMaxFilters][28];  // display name (basename, no ".png")
static int g_count = 0;

static PNG g_png;  // ~48 KB of internal RAM; lives in .bss

// Why the last filtersLoad() failed — shown in the on-screen banner.
static char g_last_err[64] = "";

// One shared decode target: full screen, LVGL "true color + alpha" layout
// (2 bytes RGB565 + 1 byte alpha per pixel). Alpha 0 = fully transparent.
static uint8_t* g_pixels = nullptr;
static lv_img_dsc_t g_dsc;

// State of the PNG currently being decoded.
static int g_off_x = 0, g_off_y = 0;  // centering offsets
static int g_type = 0;                // PNG color type

// ---------------------------------------------------------------------------
// PNGdec file callbacks (SD-backed)
// ---------------------------------------------------------------------------
static File g_file;

static void* pngOpen(const char* fn, int32_t* size) {
  g_file = SD.open(fn, FILE_READ);
  if (!g_file) return nullptr;
  *size = g_file.size();
  return &g_file;
}

static void pngClose(void* handle) {
  if (g_file) g_file.close();
}

static int32_t pngRead(PNGFILE* handle, uint8_t* buf, int32_t len) {
  return g_file.read(buf, len);
}

static int32_t pngSeek(PNGFILE* handle, int32_t pos) {
  return g_file.seek(pos) ? pos : -1;
}

// Per-line decode callback: convert any supported PNG pixel type into the
// overlay buffer. Palette PNGs (what most editors/web exports produce) carry
// their transparency in a tRNS alpha palette, which PNGdec stores at
// pPalette[768..1023].
static int pngDraw(PNGDRAW* p) {
  int y = p->y + g_off_y;
  if (y < 0 || y >= DISP_HEIGHT) return 1;
  int w = p->iWidth;
  if (g_off_x + w > DISP_WIDTH) w = DISP_WIDTH - g_off_x;

  uint8_t* dst = &g_pixels[((size_t)y * DISP_WIDTH + g_off_x) * 3];
  const uint8_t* src = (const uint8_t*)p->pPixels;

  for (int x = 0; x < w; ++x) {
    uint8_t r, g, b, a;
    switch (g_type) {
      case PNG_PIXEL_TRUECOLOR_ALPHA:  // RGBA8
        r = src[0], g = src[1], b = src[2], a = src[3];
        src += 4;
        break;
      case PNG_PIXEL_TRUECOLOR:  // RGB8, opaque
        r = src[0], g = src[1], b = src[2], a = 0xFF;
        src += 3;
        break;
      case PNG_PIXEL_GRAY_ALPHA:  // gray8 + alpha8
        r = g = b = src[0], a = src[1];
        src += 2;
        break;
      case PNG_PIXEL_GRAYSCALE:  // gray8, opaque
        r = g = b = src[0], a = 0xFF;
        src += 1;
        break;
      case PNG_PIXEL_INDEXED: {  // 1/2/4/8-bit palette indices
        int v;
        switch (p->iBpp) {
          case 8: v = src[x]; break;
          case 4: v = (src[x >> 1] >> (4 - 4 * (x & 1))) & 0xF; break;
          case 2: v = (src[x >> 2] >> (6 - 2 * (x & 3))) & 0x3; break;
          default: v = (src[x >> 3] >> (7 - (x & 7))) & 0x1; break;
        }
        const uint8_t* pal = p->pPalette;
        r = pal[v * 3], g = pal[v * 3 + 1], b = pal[v * 3 + 2];
        a = p->iHasAlpha ? pal[768 + v] : 0xFF;
        break;
      }
      default:
        r = g = b = 0, a = 0;
        break;
    }
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    dst[0] = c & 0xFF;
    dst[1] = c >> 8;
    dst[2] = a;
    dst += 3;
  }
  return 1;  // keep decoding
}

// ---------------------------------------------------------------------------
void filtersBegin() {
  g_pixels = (uint8_t*)ps_malloc((size_t)DISP_WIDTH * DISP_HEIGHT * 3);
  if (!g_pixels) {
    Serial.println("[filters] overlay buffer alloc failed");
    return;
  }

  g_dsc.header.always_zero = 0;
  g_dsc.header.w = DISP_WIDTH;
  g_dsc.header.h = DISP_HEIGHT;
  g_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
  g_dsc.data_size = (size_t)DISP_WIDTH * DISP_HEIGHT * 3;
  g_dsc.data = g_pixels;

  File dir = SD.open(FILTERS_DIR);
  if (!dir || !dir.isDirectory()) {
    Serial.println("[filters] no " FILTERS_DIR " folder on SD — no filters");
    return;
  }

  File f;
  while ((f = dir.openNextFile()) && g_count < kMaxFilters) {
    if (f.isDirectory()) continue;
    const char* n = f.name();  // basename on the 2.x core
    size_t len = strlen(n);
    if (len < 5 || strcasecmp(n + len - 4, ".png") != 0) continue;
    if (len > kMaxFileName) {
      Serial.printf("[filters] skipping %s (name longer than %u chars)\n", n,
                    (unsigned)kMaxFileName);
      continue;
    }

    snprintf(g_paths[g_count], sizeof(g_paths[0]), FILTERS_DIR "/%s", n);
    snprintf(g_names[g_count], sizeof(g_names[0]), "%.*s", (int)(len - 4), n);
    g_count++;
  }
  dir.close();

  // Alphabetical order so the rotation is predictable (FAT order is not).
  for (int i = 0; i < g_count - 1; ++i) {
    for (int j = i + 1; j < g_count; ++j) {
      if (strcasecmp(g_names[j], g_names[i]) < 0) {
        char tp[sizeof(g_paths[0])], tn[sizeof(g_names[0])];
        memcpy(tp, g_paths[i], sizeof(tp));
        memcpy(tn, g_names[i], sizeof(tn));
        memcpy(g_paths[i], g_paths[j], sizeof(tp));
        memcpy(g_names[i], g_names[j], sizeof(tn));
        memcpy(g_paths[j], tp, sizeof(tp));
        memcpy(g_names[j], tn, sizeof(tn));
      }
    }
  }

  Serial.printf("[filters] %d overlay(s) found in " FILTERS_DIR "\n", g_count);
}

int filtersCount() { return g_count; }

const char* filtersName(int idx) {
  if (idx < 0 || idx >= g_count) return "";
  return g_names[idx];
}

const char* filtersFileName(int idx) {
  if (idx < 0 || idx >= g_count) return "";
  const char* slash = strrchr(g_paths[idx], '/');
  return slash ? slash + 1 : g_paths[idx];
}

const char* filtersLastError() { return g_last_err; }

// The whole load runs SD reads through PNGdec's callbacks — hold the storage
// lock so the print task can't touch the card mid-decode.
static const lv_img_dsc_t* filtersLoadLocked(int idx);

const lv_img_dsc_t* filtersLoad(int idx) {
  storageLock();
  const lv_img_dsc_t* d = filtersLoadLocked(idx);
  storageUnlock();
  return d;
}

static const lv_img_dsc_t* filtersLoadLocked(int idx) {
  if (idx < 0 || idx >= g_count || !g_pixels) {
    snprintf(g_last_err, sizeof(g_last_err), "no filter %d", idx);
    return nullptr;
  }

  int rc = g_png.open(g_paths[idx], pngOpen, pngClose, pngRead, pngSeek,
                      pngDraw);
  if (rc != PNG_SUCCESS) {
    snprintf(g_last_err, sizeof(g_last_err), "%s: open failed (%d)",
             g_names[idx], g_png.getLastError());
    Serial.printf("[filters] %s\n", g_last_err);
    return nullptr;
  }

  int w = g_png.getWidth(), h = g_png.getHeight();
  g_type = g_png.getPixelType();
  int bpp = g_png.getBpp();

  if (w > DISP_WIDTH || h > DISP_HEIGHT) {
    snprintf(g_last_err, sizeof(g_last_err), "%s: %dx%d too big (max %dx%d)",
             g_names[idx], w, h, DISP_WIDTH, DISP_HEIGHT);
    Serial.printf("[filters] %s\n", g_last_err);
    g_png.close();
    return nullptr;
  }
  // Palette PNGs may use any index depth; everything else must be 8-bit.
  if (g_type != PNG_PIXEL_INDEXED && bpp != 8) {
    snprintf(g_last_err, sizeof(g_last_err), "%s: %d-bit PNG not supported",
             g_names[idx], bpp);
    Serial.printf("[filters] %s\n", g_last_err);
    g_png.close();
    return nullptr;
  }

  g_off_x = (DISP_WIDTH - w) / 2;
  g_off_y = (DISP_HEIGHT - h) / 2;

  // Start fully transparent so a smaller PNG floats on a clear background.
  memset(g_pixels, 0, g_dsc.data_size);

  rc = g_png.decode(nullptr, 0);
  g_png.close();
  if (rc != PNG_SUCCESS) {
    snprintf(g_last_err, sizeof(g_last_err), "%s: decode failed (%d)",
             g_names[idx], g_png.getLastError());
    Serial.printf("[filters] %s\n", g_last_err);
    return nullptr;
  }
  Serial.printf("[filters] loaded %s (%dx%d type=%d)\n", g_paths[idx], w, h,
                g_type);
  return &g_dsc;
}
