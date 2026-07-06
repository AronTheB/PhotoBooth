// =============================================================================
// cloud.cpp  —  see cloud.h
// =============================================================================
#include "cloud.h"

#include <HTTPClient.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "app.h"
#include "storage.h"

static TaskHandle_t s_task = nullptr;
static char s_session[SESSION_ID_LEN + 1] = "";
static volatile int s_count = 0;

static uint8_t* s_buf = nullptr;  // file staging buffer, PSRAM

// True only after a real round-trip to the worker succeeded. WiFi being
// connected is not enough: venue networks with login pages (and networks
// that block Cloudflare) hand out an IP but drop the traffic — showing the
// https QR there would strand guests on a dead link.
static volatile bool s_online = false;

// Filter PNGs already uploaded this boot (the worker keeps them anyway; this
// just avoids re-sending the same file every session).
static char s_sent_flt[8][32];
static int s_sent_flt_n = 0;

// ---------------------------------------------------------------------------
bool cloudReady() {
  return STA_SSID[0] && CLOUD_BASE_URL[0] && WiFi.status() == WL_CONNECTED &&
         s_online;
}

const char* cloudGalleryUrl(const char* sessionId, char* buf, size_t cap) {
  snprintf(buf, cap, "%s/s/%s", CLOUD_BASE_URL, sessionId);
  return buf;
}

// ---------------------------------------------------------------------------
// One HTTPS PUT to the worker. TLS handshakes need ~45 KB of heap for a few
// seconds; the client is scoped so it's all returned between files.
static bool putBytes(const char* upPath, const uint8_t* data, size_t len) {
  WiFiClientSecure tls;
  tls.setInsecure();  // no cert pinning; transport is still encrypted
  // The 5000 ms default socket timeout sits below real-world TLS setup time
  // on a congested AP; HTTPClient's setConnectTimeout does not reach the TLS
  // socket, so raise it on the client itself (seconds, WiFiClient semantics).
  tls.setTimeout(15);
  tls.setHandshakeTimeout(15);
  HTTPClient http;
  char url[192];
  snprintf(url, sizeof(url), "%s/up/%s?k=%s", CLOUD_BASE_URL, upPath,
           CLOUD_UPLOAD_KEY);
  if (!http.begin(tls, url)) return false;
  http.setConnectTimeout(10000);  // ESP32 TLS to a busy 2.4 GHz AP is slow
  http.setTimeout(15000);
  http.addHeader("Content-Type", "application/octet-stream");
  int code = http.PUT(const_cast<uint8_t*>(data), len);
  http.end();
  if (code != 200) Serial.printf("[cloud] PUT %s -> %d\n", upPath, code);
  return code == 200;
}

// Round-trip test: any HTTP status from the worker proves the path works.
// A login-page network or a firewall that blocks Cloudflare fails here.
static bool healthCheck() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure tls;
  tls.setInsecure();
  tls.setTimeout(15);
  tls.setHandshakeTimeout(15);
  HTTPClient http;
  if (!http.begin(tls, CLOUD_BASE_URL)) return false;
  http.setConnectTimeout(10000);
  http.setTimeout(10000);
  int code = http.GET();
  http.end();
  Serial.printf("[cloud] health check -> %d\n", code);

  if (code <= 0) {
    // Distinguish "no internet at all" (login-page network) from "Cloudflare
    // specifically blocked" (Chinese venue networks): Baidu is reachable
    // from any network in China that has real internet.
    WiFiClient plain;
    HTTPClient probe;
    if (probe.begin(plain, "http://www.baidu.com/")) {
      probe.setConnectTimeout(8000);
      probe.setTimeout(8000);
      int pc = probe.GET();
      probe.end();
      Serial.printf(
          "[cloud] internet probe (baidu) -> %d %s\n", pc,
          pc > 0 ? "(internet OK, Cloudflare is blocked on this network)"
                 : "(no internet - this WiFi likely needs a browser login)");
    }
  }
  return code > 0;
}

static bool putBytesRetry(const char* upPath, const uint8_t* d, size_t n) {
  for (int a = 0; a < 3; ++a) {
    if (putBytes(upPath, d, n)) return true;
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
  return false;
}

// Read an SD file into s_buf (holding the SD lock). 0 on failure.
static size_t readFile(const char* path) {
  storageLock();
  File f = SD.open(path, FILE_READ);
  size_t n = 0;
  if (f) {
    n = f.read(s_buf, FRAME_MAX_PAYLOAD);
    f.close();
  }
  storageUnlock();
  return n;
}

static bool filterAlreadySent(const char* name) {
  for (int i = 0; i < s_sent_flt_n; ++i)
    if (strcmp(s_sent_flt[i], name) == 0) return true;
  return false;
}

// ---------------------------------------------------------------------------
static void uploadSession() {
  char path[64], up[64];
  char tags[8][32] = {};

  // Photos first, collecting per-shot filter tags along the way.
  for (int i = 1; i <= s_count; ++i) {
    snprintf(path, sizeof(path), "/%s/%d.jpg", s_session, i);
    size_t n = readFile(path);
    if (n == 0) continue;
    snprintf(up, sizeof(up), "%s/%d.jpg", s_session, i);
    putBytesRetry(up, s_buf, n);

    snprintf(path, sizeof(path), "/%s/%d.flt", s_session, i);
    size_t fn = readFile(path);
    if (fn > 0 && fn < sizeof(tags[0])) {
      memcpy(tags[i - 1], s_buf, fn);
      tags[i - 1][fn] = '\0';
    }
  }

  // Upload each distinct filter PNG once per boot.
  for (int i = 0; i < s_count; ++i) {
    if (!tags[i][0] || filterAlreadySent(tags[i])) continue;
    snprintf(path, sizeof(path), "/filters/%s", tags[i]);
    size_t n = readFile(path);
    if (n > 0) {
      snprintf(up, sizeof(up), "f/%s", tags[i]);
      if (putBytesRetry(up, s_buf, n) && s_sent_flt_n < 8) {
        snprintf(s_sent_flt[s_sent_flt_n++], sizeof(s_sent_flt[0]), "%s",
                 tags[i]);
      }
    }
  }

  // Metadata LAST: the gallery page shows "still uploading" until this
  // lands, so guests who scan early never see broken images.
  char meta[256];
  int m = snprintf(meta, sizeof(meta), "{\"n\":%d,\"f\":{", (int)s_count);
  bool first = true;
  for (int i = 0; i < s_count; ++i) {
    if (!tags[i][0]) continue;
    m += snprintf(meta + m, sizeof(meta) - m, "%s\"%d\":\"%s\"",
                  first ? "" : ",", i + 1, tags[i]);
    first = false;
  }
  snprintf(meta + m, sizeof(meta) - m, "}}");
  snprintf(up, sizeof(up), "%s/meta", s_session);
  putBytesRetry(up, (const uint8_t*)meta, strlen(meta));

  Serial.printf("[cloud] session %s uploaded (%d photos)\n", s_session,
                (int)s_count);
}

static void cloudTask(void*) {
  for (;;) {
    // Wake on an upload request, or every 20 s to re-probe the connection —
    // that keeps cloudReady() honest, so the QR falls back to the local
    // gallery whenever the worker stops being reachable.
    bool job = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20000)) > 0;
    bool ok = healthCheck();
    if (ok != s_online) {
      s_online = ok;
      Serial.printf("[cloud] %s\n", ok ? "online — QR will be the https link"
                                       : "unreachable — local QR fallback");
    }
    if (!job) continue;
    if (!s_buf) s_buf = (uint8_t*)ps_malloc(FRAME_MAX_PAYLOAD);
    if (!s_buf || !cloudReady()) continue;
    uploadSession();
  }
}

// ---------------------------------------------------------------------------
void cloudBegin() {
  if (!STA_SSID[0] || !CLOUD_BASE_URL[0]) {
    Serial.println("[cloud] not configured — local gallery only");
    return;
  }
  // Low priority on core 0: uploads must never starve the UART link or UI.
  xTaskCreatePinnedToCore(cloudTask, "cloud", 8192, nullptr, 1, &s_task, 0);
  Serial.println("[cloud] uploader ready");
}

void cloudUploadSession(const char* sessionId, int photoCount) {
  if (!s_task || photoCount <= 0) return;
  strncpy(s_session, sessionId, SESSION_ID_LEN);
  s_session[SESSION_ID_LEN] = '\0';
  s_count = photoCount;
  xTaskNotifyGive(s_task);
}
