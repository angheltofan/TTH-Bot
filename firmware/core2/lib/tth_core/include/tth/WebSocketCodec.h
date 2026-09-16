#pragma once

#include <stddef.h>
#include <stdint.h>

// A deliberately small RFC 6455 WebSocket client codec (Step 6.2).
//
// WHY NOT esp_websocket_client: the IDF 4.4 client in this Arduino core does
// not expose the HTTP status of a failed upgrade. The gateway authenticates
// BEFORE upgrading and answers a bad device token with 401 -- and PHASE6_PLAN
// §8/D4 require AuthRejected to be handled differently from a network error
// (persistent ERROR face, retry every 5 min only). This codec sees the status.
//
// Scope, matching what tth.v1 needs and nothing more:
//   * the upgrade request (subprotocol, bearer token, device id) and a strict
//     check of the 101 response, Sec-WebSocket-Accept included;
//   * client frames: always FIN, always masked, payload < 65 536 B;
//   * server frames: unmasked, unfragmented, text/binary/close/ping/pong,
//     payload bounded by the caller's buffer. Anything else is an error, not
//     a guess.
//
// Fixed caller-supplied buffers, no heap. Portable: no Arduino, no ESP-IDF.

namespace tth {
namespace ws {

const char* const kSubprotocol = "tth.v1";

const size_t kSha1Bytes = 20;
const size_t kKeyBase64Bytes = 24;  // base64 of 16 random bytes
const size_t kAcceptBytes = 28;     // base64 of a SHA-1 digest
const size_t kMaxRequestBytes = 768;
const size_t kMaxResponseHeadBytes = 1024;
const size_t kMaxControlPayload = 125;
const size_t kMaxClientFrameHeader = 8;  // 2 + 2 (extended length) + 4 (mask)

void sha1(const uint8_t* data, size_t length, uint8_t out[kSha1Bytes]);

// Standard base64 with padding, NUL-terminated. Returns the length, or 0 if
// `capacity` cannot hold it and the terminator.
size_t base64Encode(const uint8_t* data, size_t length, char* out,
                    size_t capacity);

// Sec-WebSocket-Key from 16 random bytes.
bool makeKey(const uint8_t random16[16], char out[kKeyBase64Bytes + 1]);

// The Sec-WebSocket-Accept a server must answer for `key`.
bool computeAccept(const char* key, char out[kAcceptBytes + 1]);

struct UpgradeRequest {
  const char* host;
  uint16_t port;
  const char* path;
  const char* deviceId;
  const char* token;      // SECRET: the request buffer must be wiped after use
  const char* key;
  const char* userAgent;
};

// Returns the request length, or 0 if it does not fit or any value is empty
// or contains a control character (header injection).
size_t buildUpgradeRequest(const UpgradeRequest& request, char* out,
                           size_t capacity);

enum class HandshakeOutcome : uint8_t {
  Incomplete = 0,  // no blank line yet
  Accepted,        // 101 with every required header correct
  HttpStatus,      // a complete response that is not 101 (see `status`)
  Malformed,
  TooLong,         // no end of headers within kMaxResponseHeadBytes
  BadUpgrade,      // 101 without Upgrade: websocket / Connection: upgrade
  BadAccept,       // 101 with a missing or wrong Sec-WebSocket-Accept
  BadProtocol,     // 101 without our subprotocol
};

const char* toString(HandshakeOutcome outcome);

struct HandshakeResult {
  HandshakeOutcome outcome;
  uint16_t status;   // valid for Accepted and HttpStatus
  size_t headBytes;  // bytes up to and including the blank line
};

HandshakeResult parseUpgradeResponse(const char* data, size_t length,
                                     const char* expectedAccept,
                                     const char* expectedProtocol);

enum class Opcode : uint8_t {
  Continuation = 0x0,
  Text = 0x1,
  Binary = 0x2,
  Close = 0x8,
  Ping = 0x9,
  Pong = 0xA,
};

const char* toString(Opcode opcode);

// One complete, masked client frame. Returns its length, or 0 if it does not
// fit `capacity`, the payload is >= 65 536 B, or a control payload is > 125 B.
size_t encodeClientFrame(Opcode opcode, const uint8_t* payload, size_t length,
                         const uint8_t mask[4], uint8_t* out, size_t capacity);

enum class DecodeStatus : uint8_t { NeedMore = 0, Frame, Error };

enum class DecodeError : uint8_t {
  None = 0,
  Masked,          // servers must not mask
  ReservedBits,
  Fragmented,      // FIN clear, or a continuation frame
  UnknownOpcode,
  ControlTooLong,  // control payload > 125
  TooLarge,        // payload beyond the buffer (or a 64-bit length)
};

const char* toString(DecodeError error);

struct Frame {
  Opcode opcode;
  const uint8_t* payload;  // valid until the next feed() or reset()
  size_t length;
};

// Incremental server-frame decoder over a fixed buffer.
class FrameDecoder {
 public:
  FrameDecoder(uint8_t* buffer, size_t capacity);

  // Consumes bytes from `data` up to the end of at most one frame; `consumed`
  // says how many. Frame: frame() is complete. Error: sticky until reset().
  DecodeStatus feed(const uint8_t* data, size_t length, size_t& consumed);

  const Frame& frame() const { return _frame; }
  DecodeError error() const { return _error; }
  void reset();

 private:
  enum class Phase : uint8_t { Header, ExtendedLength, Payload };

  DecodeStatus fail(DecodeError error);
  DecodeStatus complete();

  uint8_t* _buffer;
  size_t _capacity;
  Phase _phase;
  uint8_t _header[2];
  uint8_t _headerFill;
  uint8_t _ext[2];
  uint8_t _extFill;
  size_t _expected;
  size_t _fill;
  bool _haveFrame;
  Frame _frame;
  DecodeError _error;
};

// The status code of a close frame, or 1005 (no status) when absent.
uint16_t closeCode(const Frame& frame);

}  // namespace ws
}  // namespace tth
