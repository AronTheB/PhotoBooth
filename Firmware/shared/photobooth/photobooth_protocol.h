// =============================================================================
// photobooth_protocol.h  —  The UART wire contract (build plan §4).
//
// A raw UART byte stream has no message boundaries, so every message is framed:
//
//   +------+------+------+---------------+-----------+----------+
//   | 0xA5 | 0x5A | TYPE | LENGTH (u32)  | PAYLOAD   | CRC32    |
//   | 1 B  | 1 B  | 1 B  | 4 B LE        | LENGTH B  | 4 B LE   |
//   +------+------+------+---------------+-----------+----------+
//
//   * 0xA5 0x5A  magic start bytes; the receiver hunts for these to resync.
//   * TYPE       message type (MsgType below).
//   * LENGTH     payload length in bytes, little-endian u32.
//   * PAYLOAD    TYPE-dependent, LENGTH bytes.
//   * CRC32      CRC-32 (IEEE 802.3) over TYPE || LENGTH || PAYLOAD,
//                stored little-endian. Mismatch => drop frame and resync.
//
// This header is shared verbatim by both firmwares — it IS the contract, so it
// is implemented once here and never duplicated. See photobooth_protocol.cpp.
// =============================================================================
#pragma once

#include <Arduino.h>
#include <functional>

#include "photobooth_config.h"

// ---- Message types (build plan §4) -----------------------------------------
enum MsgType : uint8_t {
  MSG_PREVIEW_FRAME = 0x01,  // XIAO -> VIEWE : low-res preview JPEG
  MSG_CAPTURE_FRAME = 0x02,  // XIAO -> VIEWE : full-res still JPEG
  MSG_CMD_START_PREVIEW = 0x10,  // VIEWE -> XIAO : (no payload)
  MSG_CMD_STOP_PREVIEW = 0x11,   // VIEWE -> XIAO : (no payload)
  MSG_CMD_CAPTURE = 0x12,        // VIEWE -> XIAO : 1 byte resolution enum (opt)
  MSG_ACK = 0x20,                // XIAO -> VIEWE : 1 byte = TYPE being acked
  MSG_ERR = 0x21,                // XIAO -> VIEWE : 1 byte err code + opt message
};

// Fixed overhead of a frame on the wire: magic(2)+type(1)+length(4)+crc(4).
static const size_t FRAME_OVERHEAD = 11;

// ---------------------------------------------------------------------------
// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320). Same on both sides.
//   crc32(0, ...) then feed the type, the 4 length bytes, then the payload.
//   Convenience helper crc32_frame() does exactly that in one call.
// ---------------------------------------------------------------------------
uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len);
uint32_t crc32_frame(uint8_t type, uint32_t length, const uint8_t* payload);

// ---------------------------------------------------------------------------
// Sending: build a frame and write it to a Stream (e.g. Serial1) in one shot.
// Returns true if the whole frame was handed to the Stream. `payload` may be
// nullptr when `length` is 0.
// ---------------------------------------------------------------------------
bool writeFrame(Stream& out, uint8_t type, const uint8_t* payload,
                uint32_t length);

// Convenience wrappers for the common zero/one-byte control messages.
bool writeCommand(Stream& out, uint8_t type);            // no payload
bool writeByteFrame(Stream& out, uint8_t type, uint8_t b);  // 1-byte payload

// ---------------------------------------------------------------------------
// Receiving: a byte-at-a-time framing state machine with resync.
//
// Feed every received byte to feed(). When a complete, CRC-valid frame arrives,
// the onFrame callback fires with (type, payload, length). The payload pointer
// is only valid for the duration of the callback — copy anything you need to
// keep. Corrupt frames (bad CRC or over-length) are dropped silently and the
// parser resyncs on the next magic-byte pair.
//
// The parser owns a heap buffer of `capacity` bytes for the payload; allocate
// it from PSRAM on the ESP32 by passing a PSRAM pointer, or let begin()
// allocate with ps_malloc.
// ---------------------------------------------------------------------------
class FrameParser {
 public:
  using FrameHandler =
      std::function<void(uint8_t type, const uint8_t* payload, uint32_t len)>;

  FrameParser() = default;
  ~FrameParser();

  // Allocate the payload buffer (from PSRAM if available). Call once in setup.
  // Returns false if allocation failed.
  bool begin(size_t capacity = FRAME_MAX_PAYLOAD);

  void onFrame(FrameHandler handler) { handler_ = std::move(handler); }

  // Feed a single received byte. Fires onFrame() when a frame completes.
  void feed(uint8_t byte);

  // Feed a buffer of received bytes (convenience).
  void feed(const uint8_t* data, size_t len);

  // Diagnostics — useful during milestone 1 to confirm the link is healthy.
  uint32_t framesOk() const { return frames_ok_; }
  uint32_t framesBadCrc() const { return frames_bad_crc_; }
  uint32_t framesOversize() const { return frames_oversize_; }

 private:
  enum State : uint8_t {
    S_MAGIC0,
    S_MAGIC1,
    S_TYPE,
    S_LEN,
    S_PAYLOAD,
    S_CRC,
  };

  void reset();  // back to hunting for magic

  FrameHandler handler_;
  uint8_t* buf_ = nullptr;
  size_t capacity_ = 0;

  State state_ = S_MAGIC0;
  uint8_t type_ = 0;
  uint32_t length_ = 0;
  uint32_t payload_pos_ = 0;
  uint8_t len_bytes_[4];
  uint8_t crc_bytes_[4];
  uint8_t field_pos_ = 0;  // index within LEN / CRC multi-byte fields

  uint32_t frames_ok_ = 0;
  uint32_t frames_bad_crc_ = 0;
  uint32_t frames_oversize_ = 0;
};
