// =============================================================================
// web_server.cpp  —  see web_server.h
// =============================================================================
#include "web_server.h"

#include <SD.h>
#include <WebServer.h>
#include <WiFi.h>

#include "app.h"
#include "photobooth_config.h"
#include "storage.h"

static WebServer g_server(HTTP_PORT);

// The session a freshly-joined phone should land on ("" = none yet).
static char g_active_session[SESSION_ID_LEN + 1] = "";

void webServerSetActiveSession(const char* id) {
  snprintf(g_active_session, sizeof(g_active_session), "%s", id);
}

// ---- Helpers ---------------------------------------------------------------

// Is `id` a plausible session id? Reject anything with path separators or
// funny characters so a request can never escape the session folder.
static bool validSessionId(const String& id) {
  if (id.length() == 0 || id.length() > 8) return false;
  for (size_t i = 0; i < id.length(); ++i) {
    char c = id[i];
    if (!isalnum((int)c)) return false;
  }
  return true;
}

// Is `name` a safe filter PNG filename ("hearts.png")? Same idea as
// validSessionId: nothing that could walk out of /filters.
static bool validFilterName(const String& name) {
  if (name.length() < 5 || name.length() > 31) return false;
  if (!name.endsWith(".png")) return false;
  for (size_t i = 0; i < name.length(); ++i) {
    char c = name[i];
    if (!isalnum((int)c) && c != '.' && c != '_' && c != '-') return false;
  }
  return true;
}

// Read the /<id>/<n>.flt sidecar (filter PNG filename for that shot).
// Returns true and fills `out` only if the tag exists and is valid.
static bool readFilterTag(const String& id, int n, char* out, size_t cap) {
  char path[32];
  snprintf(path, sizeof(path), "/%s/%d.flt", id.c_str(), n);
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  size_t got = f.readBytes(out, cap - 1);
  f.close();
  out[got] = '\0';
  return validFilterName(String(out));
}

// Count /<id>/<n>.jpg files present, scanning n = 1..maxShots.
static int countPhotos(const String& id) {
  int count = 0;
  for (int n = 1; n <= 8; ++n) {
    char path[32];
    snprintf(path, sizeof(path), "/%s/%d.jpg", id.c_str(), n);
    if (SD.exists(path)) count++;
    else break;  // photos are contiguous 1..N
  }
  return count;
}

// GET /s/<id> -> gallery HTML. The frame/decoration lives in this CSS, so the
// ESP32 never composites images (build plan §9).
static void sendGallery(const String& id) {
  int n = countPhotos(id);
  if (n == 0) {
    g_server.send(404, "text/plain", "session not found");
    return;
  }

  String html;
  html.reserve(1024 + n * 128);
  html += F(
      "<!doctype html><html><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>PhotoBooth</title><style>"
      "body{margin:0;background:#111;color:#eee;font-family:system-ui,sans-serif;"
      "text-align:center}"
      "h1{padding:16px;font-weight:600}"
      ".grid{display:grid;gap:12px;padding:12px;"
      "grid-template-columns:repeat(auto-fit,minmax(240px,1fr))}"
      // The white padding + shadow here IS the printed-strip frame.
      ".photo{background:#fff;padding:10px 10px 40px;border-radius:6px;"
      "box-shadow:0 4px 20px rgba(0,0,0,.5)}"
      ".photo img{width:100%;display:block;border-radius:2px}"
      "a.dl{display:inline-block;margin:20px;padding:12px 24px;background:#0a84ff;"
      "color:#fff;border-radius:8px;text-decoration:none;font-weight:600}"
      "</style></head><body><h1>Your photos</h1><div class=grid>");

  for (int i = 1; i <= n; ++i) {
    html += F("<div class=photo><img src='/s/");
    html += id;
    html += '/';
    html += i;
    html += F(".jpg'");
    char flt[32];
    if (readFilterTag(id, i, flt, sizeof(flt))) {
      html += F(" data-flt='/f/");
      html += flt;
      html += '\'';
    }
    html += F("></div>");
  }
  html += F("</div><p>Tap and hold a photo to save it.</p>");

  // WYSIWYG: the 800x480 screen shows only the vertically centered band of
  // the 4:3 sensor frame, so crop every photo to that same band client-side —
  // the download then matches what the guest posed with. Shots taken with a
  // filter carry data-flt; the overlay is drawn 1:1 onto the cropped band, so
  // it lands exactly where it appeared on the booth screen. The untouched
  // 4:3 originals stay on the SD card.
  html += F(
      "<script>document.querySelectorAll('.photo img').forEach("
      "function(im){var ph=new Image(),fl=null,n=0,need=1;"
      "var fu=im.getAttribute('data-flt');"
      "if(fu){fl=new Image();need=2;}"
      "function go(){if(++n<need)return;"
      "var c=document.createElement('canvas');"
      "var cw=ph.naturalWidth,chh=cw*480/800;"  // screen-shaped band
      "c.width=cw;c.height=chh;"
      "var x=c.getContext('2d');"
      "x.drawImage(ph,0,(ph.naturalHeight-chh)/2,cw,chh,0,0,cw,chh);"
      "if(fl){var s=cw/800;"  // screen px -> photo px
      "var fx=(800-fl.naturalWidth)/2,fy=(480-fl.naturalHeight)/2;"
      "x.drawImage(fl,fx*s,fy*s,fl.naturalWidth*s,fl.naturalHeight*s);}"
      "im.src=c.toDataURL('image/jpeg',0.92);}"
      "ph.onload=go;ph.src=im.src;"
      "if(fl){fl.onload=go;fl.src=fu;}});</script>"
      "</body></html>");

  g_server.send(200, "text/html", html);
}

// GET /s/<id>/<n>.jpg -> stream the raw file from SD.
static void sendPhoto(const String& id, const String& file) {
  char path[40];
  snprintf(path, sizeof(path), "/%s/%s", id.c_str(), file.c_str());
  File f = SD.open(path, FILE_READ);
  if (!f) {
    g_server.send(404, "text/plain", "not found");
    return;
  }
  g_server.streamFile(f, "image/jpeg");
  f.close();
}

// Route parser: matches /s/<id>, /s/<id>/<file>, and /f/<filter.png>.
// Every branch below touches the SD, and the print task reads it from the
// other core — hold the storage lock for the whole request.
static void handleRequestLocked();

static void handleRequest() {
  storageLock();
  handleRequestLocked();
  storageUnlock();
}

static void handleRequestLocked() {
  String uri = g_server.uri();  // e.g. "/s/A1B2/1.jpg"

  // Filter overlay PNGs for client-side compositing.
  if (uri.startsWith("/f/")) {
    String name = uri.substring(3);
    if (!validFilterName(name)) {
      g_server.send(400, "text/plain", "bad filter");
      return;
    }
    String path = "/filters/" + name;
    File f = SD.open(path, FILE_READ);
    if (!f) {
      g_server.send(404, "text/plain", "not found");
      return;
    }
    g_server.streamFile(f, "image/png");
    f.close();
    return;
  }

  if (!uri.startsWith("/s/")) {
    // "/" and anything unknown: land on the newest gallery if there is one.
    if (g_active_session[0]) {
      g_server.sendHeader(
          "Location", String("http://") + AP_IP + "/s/" + g_active_session);
      g_server.send(302, "text/plain", "");
    } else {
      g_server.send(200, "text/html",
                    "<!doctype html><meta name=viewport "
                    "content='width=device-width,initial-scale=1'>"
                    "<body style='font-family:sans-serif;text-align:center;"
                    "background:#111;color:#eee'><h2>PhotoBooth</h2>"
                    "<p>No photos yet &mdash; take one at the booth!</p>");
    }
    return;
  }
  String rest = uri.substring(3);  // "A1B2/1.jpg" or "A1B2"
  int slash = rest.indexOf('/');
  String id = (slash < 0) ? rest : rest.substring(0, slash);

  if (!validSessionId(id)) {
    g_server.send(400, "text/plain", "bad session");
    return;
  }
  if (slash < 0) {
    sendGallery(id);
  } else {
    String file = rest.substring(slash + 1);
    // Only allow "<n>.jpg" — no subpaths, no traversal.
    if (file.indexOf('/') >= 0 || !file.endsWith(".jpg")) {
      g_server.send(400, "text/plain", "bad file");
      return;
    }
    sendPhoto(id, file);
  }
}

// ---------------------------------------------------------------------------
static void apStart() {
  // AP+STA when a cloud uplink is configured: the AP stays as the offline
  // fallback gallery while the STA joins the internet-connected WiFi that
  // the cloud uploader uses.
  WiFi.mode(STA_SSID[0] ? WIFI_AP_STA : WIFI_AP);
  IPAddress ip;
  ip.fromString(AP_IP);
  IPAddress gw = ip;
  IPAddress mask(255, 255, 255, 0);
  WiFi.softAPConfig(ip, gw, mask);
  bool ok = (strlen(AP_PASSWORD) == 0) ? WiFi.softAP(AP_SSID)
                                       : WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.printf("[web] softAP %s -> %s (%s)\n", AP_SSID,
                WiFi.softAPIP().toString().c_str(), ok ? "up" : "FAILED");
  if (STA_SSID[0]) {
    WiFi.setAutoReconnect(true);
    WiFi.begin(STA_SSID, STA_PASSWORD);
    Serial.printf("[web] joining upstream WiFi '%s'...\n", STA_SSID);
  }
}

void webServerPauseAP() {
  Serial.println("[web] pausing softAP for BLE");
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
}

void webServerResumeAP() { apStart(); }

void webServerBegin() {
  apStart();
  g_server.onNotFound(handleRequest);  // all /s/... routing handled here
  g_server.begin();
}

void webServerLoop() { g_server.handleClient(); }
