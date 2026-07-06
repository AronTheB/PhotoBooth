// =============================================================================
// link_diag.cpp  —  Milestone-1 UART link proof, VIEWE side (build plan §10.1).
//
// Built ONLY in the `diag` environment (pio run -e diag -t upload). No display,
// SD, or WiFi. Parses frames from the XIAO, prints the counter plus running
// OK/badCRC/oversize counts, and ACKs each heartbeat back so the XIAO can prove
// the reverse direction too.
// =============================================================================
#include <Arduino.h>

#include "photobooth_protocol.h"

static const uint8_t MSG_HEARTBEAT = 0x30;

static HardwareSerial& Link = Serial1;
static FrameParser g_parser;

static void onFrame(uint8_t type, const uint8_t* payload, uint32_t len) {
  if (type == MSG_HEARTBEAT && len >= 4) {
    uint32_t counter = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8) |
                       ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
    Serial.printf("[diag] <- heartbeat %u   ok=%u badCRC=%u oversize=%u\n",
                  (unsigned)counter, (unsigned)g_parser.framesOk(),
                  (unsigned)g_parser.framesBadCrc(),
                  (unsigned)g_parser.framesOversize());
    // Prove the reverse direction: ACK the heartbeat back to the XIAO.
    writeByteFrame(Link, MSG_ACK, MSG_HEARTBEAT);
  } else {
    Serial.printf("[diag] <- frame type 0x%02X len %u\n", type, (unsigned)len);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[diag] VIEWE link receiver — waiting for heartbeats");
  Serial.printf("[diag] UART on RX=GPIO%d TX=GPIO%d @ %d baud\n",
                VIEWE_UART_RX_PIN, VIEWE_UART_TX_PIN, UART_BAUD);

  Link.begin(UART_BAUD, SERIAL_8N1, VIEWE_UART_RX_PIN, VIEWE_UART_TX_PIN);
  // Same RX FIFO-full threshold as the real app (see uart_link.cpp): the
  // core's default of 120/128 leaves only ~80 us of ISR-latency headroom
  // at 1 Mbaud before the driver drops the FIFO on overflow.
  Link.setRxFIFOFull(8);
  g_parser.begin();
  g_parser.onFrame(onFrame);
}

void loop() {
  while (Link.available()) g_parser.feed((uint8_t)Link.read());
}
