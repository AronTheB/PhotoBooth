// =============================================================================
// link_diag.cpp  —  Milestone-1 UART link proof, XIAO side (build plan §10.1).
//
// Built ONLY in the `diag` environment (pio run -e diag -t upload). No camera.
// Sends a framed counter once per second and prints any ACK the VIEWE sends
// back — so you can confirm framing + CRC + BOTH directions of the wire before
// anything else is layered on.
// =============================================================================
#include <Arduino.h>

#include "photobooth_protocol.h"

// Local diagnostic message type (not part of the app protocol enum).
static const uint8_t MSG_HEARTBEAT = 0x30;

static HardwareSerial& Link = Serial1;
static FrameParser g_parser;
static uint32_t g_counter = 0;
static uint32_t g_last_send = 0;

// Handle frames coming back from the VIEWE (we expect ACKs).
static void onFrame(uint8_t type, const uint8_t* payload, uint32_t len) {
  if (type == MSG_ACK && len >= 1) {
    Serial.printf("[diag] <- ACK for type 0x%02X\n", payload[0]);
  } else {
    Serial.printf("[diag] <- frame type 0x%02X len %u\n", type, (unsigned)len);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[diag] XIAO link sender — heartbeat every 1s");

  Link.begin(UART_BAUD, SERIAL_8N1, XIAO_UART_RX_PIN, XIAO_UART_TX_PIN);
  g_parser.begin();
  g_parser.onFrame(onFrame);
}

void loop() {
  // Drain inbound bytes (ACKs from the VIEWE).
  while (Link.available()) g_parser.feed((uint8_t)Link.read());

  // Send a heartbeat once per second with a rolling counter payload.
  uint32_t now = millis();
  if (now - g_last_send >= 1000) {
    g_last_send = now;
    uint8_t p[4] = {(uint8_t)(g_counter), (uint8_t)(g_counter >> 8),
                    (uint8_t)(g_counter >> 16), (uint8_t)(g_counter >> 24)};
    writeFrame(Link, MSG_HEARTBEAT, p, sizeof(p));
    Serial.printf("[diag] -> heartbeat %u\n", (unsigned)g_counter);
    g_counter++;
  }
}
