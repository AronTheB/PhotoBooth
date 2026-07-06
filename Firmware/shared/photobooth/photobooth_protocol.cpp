// =============================================================================
// photobooth_protocol.cpp  —  Implementation of the UART wire contract.
// See photobooth_protocol.h for the frame layout and rationale.
// =============================================================================
#include "photobooth_protocol.h"

// ---------------------------------------------------------------------------
// CRC-32 (IEEE 802.3, reflected). Table is built once on first use so we don't
// pay for a 1 KB const table in flash and it stays simple.
// ---------------------------------------------------------------------------
static uint32_t s_crc_table[256];
static bool s_crc_table_ready = false;

static void crc32_init_table() {
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    s_crc_table[i] = c;
  }
  s_crc_table_ready = true;
}

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
  if (!s_crc_table_ready) crc32_init_table();
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) {
    crc = s_crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

uint32_t crc32_frame(uint8_t type, uint32_t length, const uint8_t* payload) {
  uint8_t header[5];
  header[0] = type;
  header[1] = (uint8_t)(length & 0xFF);
  header[2] = (uint8_t)((length >> 8) & 0xFF);
  header[3] = (uint8_t)((length >> 16) & 0xFF);
  header[4] = (uint8_t)((length >> 24) & 0xFF);
  uint32_t crc = crc32_update(0, header, sizeof(header));
  if (length && payload) crc = crc32_update(crc, payload, length);
  return crc;
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------
bool writeFrame(Stream& out, uint8_t type, const uint8_t* payload,
                uint32_t length) {
  uint8_t header[7];
  header[0] = FRAME_MAGIC0;
  header[1] = FRAME_MAGIC1;
  header[2] = type;
  header[3] = (uint8_t)(length & 0xFF);
  header[4] = (uint8_t)((length >> 8) & 0xFF);
  header[5] = (uint8_t)((length >> 16) & 0xFF);
  header[6] = (uint8_t)((length >> 24) & 0xFF);

  uint32_t crc = crc32_frame(type, length, payload);
  uint8_t crc_bytes[4] = {
      (uint8_t)(crc & 0xFF), (uint8_t)((crc >> 8) & 0xFF),
      (uint8_t)((crc >> 16) & 0xFF), (uint8_t)((crc >> 24) & 0xFF)};

  // Write header, then payload, then CRC. Stream::write blocks until the UART
  // TX buffer can accept the bytes; callers that must not block (the preview
  // loop) should check availableForWrite() before calling.
  if (out.write(header, sizeof(header)) != sizeof(header)) return false;
  if (length && payload) {
    if (out.write(payload, length) != length) return false;
  }
  if (out.write(crc_bytes, sizeof(crc_bytes)) != sizeof(crc_bytes)) return false;
  return true;
}

bool writeCommand(Stream& out, uint8_t type) {
  return writeFrame(out, type, nullptr, 0);
}

bool writeByteFrame(Stream& out, uint8_t type, uint8_t b) {
  return writeFrame(out, type, &b, 1);
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------
FrameParser::~FrameParser() {
  if (buf_) free(buf_);
}

bool FrameParser::begin(size_t capacity) {
  capacity_ = capacity;
#if defined(BOARD_HAS_PSRAM) || defined(CONFIG_SPIRAM)
  buf_ = (uint8_t*)ps_malloc(capacity_);
#endif
  if (!buf_) buf_ = (uint8_t*)malloc(capacity_);  // fall back to internal RAM
  reset();
  return buf_ != nullptr;
}

void FrameParser::reset() {
  state_ = S_MAGIC0;
  type_ = 0;
  length_ = 0;
  payload_pos_ = 0;
  field_pos_ = 0;
}

void FrameParser::feed(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) feed(data[i]);
}

void FrameParser::feed(uint8_t byte) {
  switch (state_) {
    case S_MAGIC0:
      if (byte == FRAME_MAGIC0) state_ = S_MAGIC1;
      break;

    case S_MAGIC1:
      if (byte == FRAME_MAGIC1) {
        state_ = S_TYPE;
      } else if (byte == FRAME_MAGIC0) {
        // Two 0xA5 in a row: stay poised for the 0x5A.
        state_ = S_MAGIC1;
      } else {
        state_ = S_MAGIC0;
      }
      break;

    case S_TYPE:
      type_ = byte;
      field_pos_ = 0;
      state_ = S_LEN;
      break;

    case S_LEN:
      len_bytes_[field_pos_++] = byte;
      if (field_pos_ == 4) {
        length_ = (uint32_t)len_bytes_[0] | ((uint32_t)len_bytes_[1] << 8) |
                  ((uint32_t)len_bytes_[2] << 16) |
                  ((uint32_t)len_bytes_[3] << 24);
        // Guard against a corrupt/huge length. Drop and resync.
        if (length_ > capacity_) {
          frames_oversize_++;
          reset();
          break;
        }
        payload_pos_ = 0;
        field_pos_ = 0;
        state_ = (length_ == 0) ? S_CRC : S_PAYLOAD;
      }
      break;

    case S_PAYLOAD:
      buf_[payload_pos_++] = byte;
      if (payload_pos_ == length_) {
        field_pos_ = 0;
        state_ = S_CRC;
      }
      break;

    case S_CRC:
      crc_bytes_[field_pos_++] = byte;
      if (field_pos_ == 4) {
        uint32_t got = (uint32_t)crc_bytes_[0] | ((uint32_t)crc_bytes_[1] << 8) |
                       ((uint32_t)crc_bytes_[2] << 16) |
                       ((uint32_t)crc_bytes_[3] << 24);
        uint32_t want = crc32_frame(type_, length_, buf_);
        if (got == want) {
          frames_ok_++;
          if (handler_) handler_(type_, buf_, length_);
        } else {
          frames_bad_crc_++;
        }
        reset();
      }
      break;
  }
}
