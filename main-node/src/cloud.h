// =============================================================================
// cloud.h  —  Session upload to the Cloudflare Worker gallery.
//
// When the booth has internet (STA_SSID joined, CLOUD_BASE_URL set), sessions
// upload in the background and the QR becomes a public https link — guests
// never join the booth WiFi. When offline, cloudReady() is false and the UI
// falls back to the local-AP gallery.
// =============================================================================
#pragma once

#include <Arduino.h>

// Start the uploader task. Call once from setup(), after webServerBegin()
// (which also brings the STA connection up when STA_SSID is set).
void cloudBegin();

// True when the cloud path is configured AND the upstream WiFi is connected.
bool cloudReady();

// Build the public gallery URL for a session into buf.
const char* cloudGalleryUrl(const char* sessionId, char* buf, size_t cap);

// Queue a finished session for background upload (photos + filter tags +
// the filter PNGs themselves). Non-blocking.
void cloudUploadSession(const char* sessionId, int photoCount);
