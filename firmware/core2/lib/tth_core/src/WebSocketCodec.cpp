#include "tth/WebSocketCodec.h"

#include <stdio.h>
#include <string.h>

namespace tth {
namespace ws {

// --- SHA-1 (FIPS 180-4), for Sec-WebSocket-Accept only ---------------------------

namespace {

uint32_t rotl(uint32_t value, int bits) {
  return (value << bits) | (value >> (32 - bits));
}

void sha1Block(uint32_t h[5], const uint8_t* block) {
  uint32_t w[80];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
           (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 80; ++i) {
    w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; ++i) {
    uint32_t f;
    uint32_t k;
    if (i < 20) {
      f = (b & c) | ((~b) & d);
      k = 0x5A827999u;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1u;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDCu;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6u;
    }
    const uint32_t temp = rotl(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rotl(b, 30);
    b = a;
    a = temp;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
}

}  // namespace

void sha1(const uint8_t* data, size_t length, uint8_t out[kSha1Bytes]) {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                   0xC3D2E1F0u};
  size_t offset = 0;
  while (length - offset >= 64) {
    sha1Block(h, data + offset);
    offset += 64;
  }
  uint8_t tail[128];
  memset(tail, 0, sizeof(tail));
  const size_t rest = length - offset;
  if (rest > 0) memcpy(tail, data + offset, rest);
  tail[rest] = 0x80;
  const size_t tailBlocks = (rest + 1 + 8 <= 64) ? 1 : 2;
  const uint64_t bits = static_cast<uint64_t>(length) * 8u;
  for (int i = 0; i < 8; ++i) {
    tail[tailBlocks * 64 - 1 - i] = static_cast<uint8_t>(bits >> (8 * i));
  }
  for (size_t i = 0; i < tailBlocks; ++i) sha1Block(h, tail + i * 64);
  for (int i = 0; i < 5; ++i) {
    out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
    out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
    out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
    out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
  }
}

size_t base64Encode(const uint8_t* data, size_t length, char* out,
                    size_t capacity) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const size_t need = ((length + 2) / 3) * 4;
  if (out == nullptr || capacity < need + 1) return 0;
  size_t o = 0;
  for (size_t i = 0; i < length; i += 3) {
    const uint32_t b0 = data[i];
    const uint32_t b1 = (i + 1 < length) ? data[i + 1] : 0;
    const uint32_t b2 = (i + 2 < length) ? data[i + 2] : 0;
    const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
    out[o++] = kAlphabet[(triple >> 18) & 0x3F];
    out[o++] = kAlphabet[(triple >> 12) & 0x3F];
    out[o++] = (i + 1 < length) ? kAlphabet[(triple >> 6) & 0x3F] : '=';
    out[o++] = (i + 2 < length) ? kAlphabet[triple & 0x3F] : '=';
  }
  out[o] = '\0';
  return o;
}

bool makeKey(const uint8_t random16[16], char out[kKeyBase64Bytes + 1]) {
  return base64Encode(random16, 16, out, kKeyBase64Bytes + 1) == kKeyBase64Bytes;
}

bool computeAccept(const char* key, char out[kAcceptBytes + 1]) {
  static const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  if (key == nullptr) return false;
  const size_t keyLength = strlen(key);
  if (keyLength == 0 || keyLength > 64) return false;
  char joined[64 + sizeof(kGuid)];
  memcpy(joined, key, keyLength);
  memcpy(joined + keyLength, kGuid, sizeof(kGuid) - 1);
  uint8_t digest[kSha1Bytes];
  sha1(reinterpret_cast<const uint8_t*>(joined), keyLength + sizeof(kGuid) - 1,
       digest);
  return base64Encode(digest, kSha1Bytes, out, kAcceptBytes + 1) == kAcceptBytes;
}

// --- upgrade request ----------------------------------------------------------------

namespace {

bool headerSafe(const char* value) {
  if (value == nullptr || value[0] == '\0') return false;
  for (const char* p = value; *p != '\0'; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c < 0x20 || c == 0x7F) return false;
  }
  return true;
}

}  // namespace

size_t buildUpgradeRequest(const UpgradeRequest& r, char* out, size_t capacity) {
  if (out == nullptr || capacity == 0) return 0;
  if (!headerSafe(r.host) || !headerSafe(r.path) || !headerSafe(r.deviceId) ||
      !headerSafe(r.token) || !headerSafe(r.key) || !headerSafe(r.userAgent) ||
      r.path[0] != '/' || r.port == 0) {
    return 0;
  }
  char hostHeader[270];
  if (r.port == 443) {
    snprintf(hostHeader, sizeof(hostHeader), "%s", r.host);
  } else {
    snprintf(hostHeader, sizeof(hostHeader), "%s:%u", r.host,
             static_cast<unsigned>(r.port));
  }
  const int written = snprintf(
      out, capacity,
      "GET %s HTTP/1.1\r\n"
      "Host: %s\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: %s\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "Sec-WebSocket-Protocol: %s\r\n"
      "Authorization: Bearer %s\r\n"
      "X-TTH-Device: %s\r\n"
      "User-Agent: %s\r\n"
      "\r\n",
      r.path, hostHeader, r.key, kSubprotocol, r.token, r.deviceId, r.userAgent);
  if (written <= 0 || static_cast<size_t>(written) >= capacity) {
    memset(out, 0, capacity);  // never leave a truncated token behind
    return 0;
  }
  return static_cast<size_t>(written);
}

// --- upgrade response --------------------------------------------------------------

const char* toString(HandshakeOutcome outcome) {
  switch (outcome) {
    case HandshakeOutcome::Incomplete:
      return "incomplete";
    case HandshakeOutcome::Accepted:
      return "accepted";
    case HandshakeOutcome::HttpStatus:
      return "http status";
    case HandshakeOutcome::Malformed:
      return "malformed response";
    case HandshakeOutcome::TooLong:
      return "response headers too long";
    case HandshakeOutcome::BadUpgrade:
      return "missing upgrade headers";
    case HandshakeOutcome::BadAccept:
      return "wrong Sec-WebSocket-Accept";
    case HandshakeOutcome::BadProtocol:
      return "subprotocol not accepted";
  }
  return "invalid";
}

namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool equalsIgnoreCase(const char* a, size_t aLength, const char* b) {
  const size_t bLength = strlen(b);
  if (aLength != bLength) return false;
  for (size_t i = 0; i < aLength; ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

// A comma-separated header value containing `token` (case-insensitive).
bool listContains(const char* value, size_t length, const char* token) {
  size_t start = 0;
  while (start <= length) {
    size_t end = start;
    while (end < length && value[end] != ',') ++end;
    size_t s = start;
    size_t e = end;
    while (s < e && (value[s] == ' ' || value[s] == '\t')) ++s;
    while (e > s && (value[e - 1] == ' ' || value[e - 1] == '\t')) --e;
    if (equalsIgnoreCase(value + s, e - s, token)) return true;
    start = end + 1;
  }
  return false;
}

}  // namespace

HandshakeResult parseUpgradeResponse(const char* data, size_t length,
                                     const char* expectedAccept,
                                     const char* expectedProtocol) {
  HandshakeResult result;
  result.outcome = HandshakeOutcome::Incomplete;
  result.status = 0;
  result.headBytes = 0;
  if (data == nullptr) {
    result.outcome = HandshakeOutcome::Malformed;
    return result;
  }

  size_t headEnd = 0;
  for (size_t i = 0; i + 3 < length; ++i) {
    if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' &&
        data[i + 3] == '\n') {
      headEnd = i + 4;
      break;
    }
  }
  if (headEnd == 0) {
    result.outcome = (length >= kMaxResponseHeadBytes) ? HandshakeOutcome::TooLong
                                                       : HandshakeOutcome::Incomplete;
    return result;
  }
  if (headEnd > kMaxResponseHeadBytes) {
    result.outcome = HandshakeOutcome::TooLong;
    return result;
  }
  result.headBytes = headEnd;

  // Status line: HTTP/1.1 NNN[ reason]
  size_t lineEnd = 0;
  while (lineEnd + 1 < headEnd && !(data[lineEnd] == '\r' && data[lineEnd + 1] == '\n')) {
    ++lineEnd;
  }
  if (lineEnd < 12 || memcmp(data, "HTTP/1.", 7) != 0 ||
      (data[7] != '1' && data[7] != '0') || data[8] != ' ') {
    result.outcome = HandshakeOutcome::Malformed;
    return result;
  }
  for (int i = 9; i < 12; ++i) {
    if (data[i] < '0' || data[i] > '9') {
      result.outcome = HandshakeOutcome::Malformed;
      return result;
    }
  }
  if (lineEnd > 12 && data[12] != ' ') {
    result.outcome = HandshakeOutcome::Malformed;
    return result;
  }
  result.status = static_cast<uint16_t>((data[9] - '0') * 100 + (data[10] - '0') * 10 +
                                        (data[11] - '0'));

  bool upgradeOk = false;
  bool connectionOk = false;
  bool acceptOk = false;
  bool acceptSeen = false;
  bool protocolOk = false;

  size_t pos = lineEnd + 2;
  while (pos + 2 <= headEnd) {
    size_t end = pos;
    while (end + 1 < headEnd && !(data[end] == '\r' && data[end + 1] == '\n')) ++end;
    if (end == pos) break;  // the blank line
    size_t colon = pos;
    while (colon < end && data[colon] != ':') ++colon;
    if (colon == end || colon == pos) {
      result.outcome = HandshakeOutcome::Malformed;
      return result;
    }
    const char* name = data + pos;
    const size_t nameLength = colon - pos;
    size_t vs = colon + 1;
    size_t ve = end;
    while (vs < ve && (data[vs] == ' ' || data[vs] == '\t')) ++vs;
    while (ve > vs && (data[ve - 1] == ' ' || data[ve - 1] == '\t')) --ve;
    const char* value = data + vs;
    const size_t valueLength = ve - vs;

    if (equalsIgnoreCase(name, nameLength, "upgrade")) {
      upgradeOk = equalsIgnoreCase(value, valueLength, "websocket");
    } else if (equalsIgnoreCase(name, nameLength, "connection")) {
      connectionOk = listContains(value, valueLength, "upgrade");
    } else if (equalsIgnoreCase(name, nameLength, "sec-websocket-accept")) {
      acceptSeen = true;
      acceptOk = expectedAccept != nullptr && strlen(expectedAccept) == valueLength &&
                 memcmp(value, expectedAccept, valueLength) == 0;
    } else if (equalsIgnoreCase(name, nameLength, "sec-websocket-protocol")) {
      protocolOk = expectedProtocol != nullptr &&
                   strlen(expectedProtocol) == valueLength &&
                   memcmp(value, expectedProtocol, valueLength) == 0;
    }
    pos = end + 2;
  }

  if (result.status != 101) {
    result.outcome = HandshakeOutcome::HttpStatus;
    return result;
  }
  if (!upgradeOk || !connectionOk) {
    result.outcome = HandshakeOutcome::BadUpgrade;
  } else if (!acceptSeen || !acceptOk) {
    result.outcome = HandshakeOutcome::BadAccept;
  } else if (!protocolOk) {
    result.outcome = HandshakeOutcome::BadProtocol;
  } else {
    result.outcome = HandshakeOutcome::Accepted;
  }
  return result;
}

// --- frames -----------------------------------------------------------------------

const char* toString(Opcode opcode) {
  switch (opcode) {
    case Opcode::Continuation:
      return "continuation";
    case Opcode::Text:
      return "text";
    case Opcode::Binary:
      return "binary";
    case Opcode::Close:
      return "close";
    case Opcode::Ping:
      return "ping";
    case Opcode::Pong:
      return "pong";
  }
  return "invalid";
}

const char* toString(DecodeError error) {
  switch (error) {
    case DecodeError::None:
      return "none";
    case DecodeError::Masked:
      return "masked server frame";
    case DecodeError::ReservedBits:
      return "reserved bits set";
    case DecodeError::Fragmented:
      return "fragmented frame";
    case DecodeError::UnknownOpcode:
      return "unknown opcode";
    case DecodeError::ControlTooLong:
      return "control frame too long";
    case DecodeError::TooLarge:
      return "frame too large";
  }
  return "invalid";
}

namespace {

bool isControl(Opcode opcode) {
  return opcode == Opcode::Close || opcode == Opcode::Ping || opcode == Opcode::Pong;
}

}  // namespace

size_t encodeClientFrame(Opcode opcode, const uint8_t* payload, size_t length,
                         const uint8_t mask[4], uint8_t* out, size_t capacity) {
  if (out == nullptr || mask == nullptr || (length > 0 && payload == nullptr)) return 0;
  if (opcode == Opcode::Continuation) return 0;
  if (isControl(opcode) && length > kMaxControlPayload) return 0;
  if (length >= 65536u) return 0;
  const size_t header = (length < 126) ? 6 : 8;
  if (capacity < header + length) return 0;

  out[0] = static_cast<uint8_t>(0x80u | static_cast<uint8_t>(opcode));
  size_t pos = 2;
  if (length < 126) {
    out[1] = static_cast<uint8_t>(0x80u | length);
  } else {
    out[1] = 0x80u | 126u;
    out[2] = static_cast<uint8_t>(length >> 8);
    out[3] = static_cast<uint8_t>(length & 0xFFu);
    pos = 4;
  }
  memcpy(out + pos, mask, 4);
  pos += 4;
  for (size_t i = 0; i < length; ++i) {
    out[pos + i] = static_cast<uint8_t>(payload[i] ^ mask[i % 4]);
  }
  return pos + length;
}

FrameDecoder::FrameDecoder(uint8_t* buffer, size_t capacity)
    : _buffer(buffer), _capacity(buffer == nullptr ? 0 : capacity) {
  reset();
}

void FrameDecoder::reset() {
  _phase = Phase::Header;
  _headerFill = 0;
  _extFill = 0;
  _expected = 0;
  _fill = 0;
  _haveFrame = false;
  _frame.opcode = Opcode::Continuation;
  _frame.payload = nullptr;
  _frame.length = 0;
  _error = DecodeError::None;
}

DecodeStatus FrameDecoder::fail(DecodeError error) {
  _error = error;
  return DecodeStatus::Error;
}

DecodeStatus FrameDecoder::complete() {
  _frame.payload = _buffer;
  _frame.length = _expected;
  _haveFrame = true;
  return DecodeStatus::Frame;
}

DecodeStatus FrameDecoder::feed(const uint8_t* data, size_t length,
                                size_t& consumed) {
  consumed = 0;
  if (_error != DecodeError::None) return DecodeStatus::Error;
  if (_haveFrame) {
    // The previous frame has been read; start the next one.
    _haveFrame = false;
    _phase = Phase::Header;
    _headerFill = 0;
    _extFill = 0;
    _expected = 0;
    _fill = 0;
  }

  while (consumed < length) {
    const uint8_t byte = data[consumed];
    switch (_phase) {
      case Phase::Header: {
        _header[_headerFill++] = byte;
        ++consumed;
        if (_headerFill < 2) break;
        if ((_header[0] & 0x70u) != 0) return fail(DecodeError::ReservedBits);
        const uint8_t op = _header[0] & 0x0Fu;
        if (op == 0x0) return fail(DecodeError::Fragmented);
        if (op != 0x1 && op != 0x2 && op != 0x8 && op != 0x9 && op != 0xA) {
          return fail(DecodeError::UnknownOpcode);
        }
        if ((_header[0] & 0x80u) == 0) return fail(DecodeError::Fragmented);
        if ((_header[1] & 0x80u) != 0) return fail(DecodeError::Masked);
        _frame.opcode = static_cast<Opcode>(op);
        const uint8_t len7 = _header[1] & 0x7Fu;
        if (isControl(_frame.opcode) && len7 > kMaxControlPayload) {
          return fail(DecodeError::ControlTooLong);
        }
        if (len7 == 127) return fail(DecodeError::TooLarge);
        if (len7 == 126) {
          _phase = Phase::ExtendedLength;
          break;
        }
        _expected = len7;
        if (_expected > _capacity) return fail(DecodeError::TooLarge);
        if (_expected == 0) return complete();
        _phase = Phase::Payload;
        break;
      }
      case Phase::ExtendedLength:
        _ext[_extFill++] = byte;
        ++consumed;
        if (_extFill < 2) break;
        _expected = (static_cast<size_t>(_ext[0]) << 8) | _ext[1];
        if (_expected > _capacity) return fail(DecodeError::TooLarge);
        if (_expected == 0) return complete();
        _phase = Phase::Payload;
        break;
      case Phase::Payload: {
        size_t take = _expected - _fill;
        if (take > length - consumed) take = length - consumed;
        memcpy(_buffer + _fill, data + consumed, take);
        _fill += take;
        consumed += take;
        if (_fill == _expected) return complete();
        break;
      }
    }
  }
  return DecodeStatus::NeedMore;
}

uint16_t closeCode(const Frame& frame) {
  if (frame.opcode != Opcode::Close || frame.length < 2 || frame.payload == nullptr) {
    return 1005;
  }
  return static_cast<uint16_t>((static_cast<uint16_t>(frame.payload[0]) << 8) |
                               frame.payload[1]);
}

}  // namespace ws
}  // namespace tth
