#include "tth/GatewayProtocol.h"

#include <stdio.h>
#include <string.h>

namespace tth {
namespace wire {

const char* toString(FrameError error) {
  switch (error) {
    case FrameError::None:
      return "none";
    case FrameError::TooShort:
      return "too short";
    case FrameError::BadKind:
      return "bad kind";
    case FrameError::BadFlags:
      return "bad flags";
    case FrameError::BadReserved:
      return "bad reserved";
    case FrameError::ZeroTurn:
      return "turn 0";
    case FrameError::EmptyPayload:
      return "empty payload";
    case FrameError::OddPayload:
      return "odd payload";
    case FrameError::Oversize:
      return "oversize";
  }
  return "invalid";
}

const char* toString(ControlType type) {
  switch (type) {
    case ControlType::Unknown:
      return "unknown";
    case ControlType::Ready:
      return "ready";
    case ControlType::SpeechStart:
      return "speech_start";
    case ControlType::TurnComplete:
      return "turn_complete";
    case ControlType::Interrupted:
      return "interrupted";
    case ControlType::Error:
      return "error";
    case ControlType::SessionEnd:
      return "session_end";
    case ControlType::Pong:
      return "pong";
  }
  return "invalid";
}

const char* toString(ControlError error) {
  switch (error) {
    case ControlError::None:
      return "none";
    case ControlError::TooLong:
      return "too long";
    case ControlError::Malformed:
      return "malformed";
    case ControlError::MissingType:
      return "missing type";
    case ControlError::MissingField:
      return "missing field";
    case ControlError::BadValue:
      return "bad value";
  }
  return "invalid";
}

// --- binary -------------------------------------------------------------------

bool writeAudioHeader(uint8_t* out, uint8_t kind, uint32_t turn) {
  if (out == nullptr || turn == kInvalidTurn) return false;
  if (kind != kKindUserAudio && kind != kKindModelAudio) return false;
  out[0] = kind;
  out[1] = 0;
  out[2] = 0;
  out[3] = 0;
  out[4] = static_cast<uint8_t>(turn & 0xFFu);
  out[5] = static_cast<uint8_t>((turn >> 8) & 0xFFu);
  out[6] = static_cast<uint8_t>((turn >> 16) & 0xFFu);
  out[7] = static_cast<uint8_t>((turn >> 24) & 0xFFu);
  return true;
}

FrameError parseAudioFrame(const uint8_t* data, size_t length,
                           uint8_t expectedKind, AudioFrameView& out) {
  if (data == nullptr || length < kAudioHeaderBytes) return FrameError::TooShort;
  if (data[0] != expectedKind ||
      (expectedKind != kKindUserAudio && expectedKind != kKindModelAudio)) {
    return FrameError::BadKind;
  }
  if (data[1] != 0) return FrameError::BadFlags;
  if (data[2] != 0 || data[3] != 0) return FrameError::BadReserved;

  const uint32_t turn = static_cast<uint32_t>(data[4]) |
                        (static_cast<uint32_t>(data[5]) << 8) |
                        (static_cast<uint32_t>(data[6]) << 16) |
                        (static_cast<uint32_t>(data[7]) << 24);
  if (turn == kInvalidTurn) return FrameError::ZeroTurn;

  const size_t pcmBytes = length - kAudioHeaderBytes;
  if (pcmBytes == 0) return FrameError::EmptyPayload;
  if ((pcmBytes & 1u) != 0) return FrameError::OddPayload;
  const uint32_t limit =
      (expectedKind == kKindUserAudio) ? kMaxUpPcmBytes : kMaxDownPcmBytes;
  if (pcmBytes > limit) return FrameError::Oversize;

  out.kind = data[0];
  out.turn = turn;
  out.pcm = data + kAudioHeaderBytes;
  out.pcmBytes = static_cast<uint32_t>(pcmBytes);
  return FrameError::None;
}

// --- encoders -----------------------------------------------------------------

namespace {

size_t finish(int written, size_t capacity) {
  if (written <= 0) return 0;
  const size_t length = static_cast<size_t>(written);
  if (length >= capacity || length > kMaxControlBytes) return 0;
  return length;
}

bool validFirmware(const char* firmware) {
  if (firmware == nullptr) return false;
  const size_t length = strlen(firmware);
  if (length == 0 || length > 24) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = firmware[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                    c == '-';
    if (!ok) return false;
  }
  return true;
}

}  // namespace

size_t encodeHello(char* out, size_t capacity, const char* firmware,
                   uint32_t credit) {
  if (out == nullptr || !validFirmware(firmware)) return 0;
  return finish(
      snprintf(out, capacity,
               "{\"t\":\"hello\",\"proto\":%u,\"fw\":\"%s\","
               "\"in\":\"s16le/16000/1\",\"out\":\"s16le/24000/1\","
               "\"maxDown\":%lu,\"credit\":%lu}",
               static_cast<unsigned>(kProtocolVersion), firmware,
               static_cast<unsigned long>(kMaxDownPcmBytes),
               static_cast<unsigned long>(credit)),
      capacity);
}

size_t encodeTurnStart(char* out, size_t capacity, uint32_t turn) {
  if (out == nullptr || turn == kInvalidTurn) return 0;
  return finish(snprintf(out, capacity, "{\"t\":\"turn_start\",\"turn\":%lu}",
                         static_cast<unsigned long>(turn)),
                capacity);
}

size_t encodeTurnEnd(char* out, size_t capacity, uint32_t turn,
                     uint32_t frames, uint32_t bytes) {
  if (out == nullptr || turn == kInvalidTurn) return 0;
  return finish(snprintf(out, capacity,
                         "{\"t\":\"turn_end\",\"turn\":%lu,\"frames\":%lu,"
                         "\"bytes\":%lu}",
                         static_cast<unsigned long>(turn),
                         static_cast<unsigned long>(frames),
                         static_cast<unsigned long>(bytes)),
                capacity);
}

size_t encodeCancel(char* out, size_t capacity, uint32_t turn) {
  if (out == nullptr || turn == kInvalidTurn) return 0;
  return finish(snprintf(out, capacity, "{\"t\":\"cancel\",\"turn\":%lu}",
                         static_cast<unsigned long>(turn)),
                capacity);
}

size_t encodeCredit(char* out, size_t capacity, uint32_t bytes) {
  if (out == nullptr) return 0;
  return finish(snprintf(out, capacity, "{\"t\":\"credit\",\"bytes\":%lu}",
                         static_cast<unsigned long>(bytes)),
                capacity);
}

size_t encodePing(char* out, size_t capacity, uint32_t ts) {
  if (out == nullptr) return 0;
  return finish(snprintf(out, capacity, "{\"t\":\"ping\",\"ts\":%lu}",
                         static_cast<unsigned long>(ts)),
                capacity);
}

// --- parser -------------------------------------------------------------------

namespace {

const size_t kMaxKey = 16;
const size_t kMaxString = 64;

enum class ValueKind : uint8_t { String, Number, Bool };

struct Cursor {
  const char* p;
  const char* end;
};

void skipSpace(Cursor& c) {
  while (c.p < c.end &&
         (*c.p == ' ' || *c.p == '\t' || *c.p == '\n' || *c.p == '\r')) {
    ++c.p;
  }
}

// A JSON string with \" \\ \/ escapes only; no control characters, no \u.
bool readString(Cursor& c, char* out, size_t capacity, size_t* length) {
  if (c.p >= c.end || *c.p != '"') return false;
  ++c.p;
  size_t n = 0;
  while (c.p < c.end) {
    char ch = *c.p++;
    if (ch == '"') {
      out[n] = '\0';
      *length = n;
      return true;
    }
    if (static_cast<unsigned char>(ch) < 0x20) return false;
    if (ch == '\\') {
      if (c.p >= c.end) return false;
      const char esc = *c.p++;
      if (esc != '"' && esc != '\\' && esc != '/') return false;
      ch = esc;
    }
    if (n + 1 >= capacity) return false;
    out[n++] = ch;
  }
  return false;
}

// An unsigned integer 0..4294967295 without sign, fraction or leading zeros.
bool readNumber(Cursor& c, uint32_t* value) {
  if (c.p >= c.end || *c.p < '0' || *c.p > '9') return false;
  if (*c.p == '0' && (c.p + 1) < c.end && c.p[1] >= '0' && c.p[1] <= '9') {
    return false;
  }
  uint64_t v = 0;
  while (c.p < c.end && *c.p >= '0' && *c.p <= '9') {
    v = v * 10u + static_cast<uint64_t>(*c.p - '0');
    if (v > 0xFFFFFFFFull) return false;
    ++c.p;
  }
  if (c.p < c.end && (*c.p == '.' || *c.p == 'e' || *c.p == 'E')) return false;
  *value = static_cast<uint32_t>(v);
  return true;
}

bool readLiteral(Cursor& c, const char* word) {
  const size_t n = strlen(word);
  if (static_cast<size_t>(c.end - c.p) < n) return false;
  if (memcmp(c.p, word, n) != 0) return false;
  c.p += n;
  return true;
}

bool copyField(const char* value, size_t length, char* dest, size_t capacity) {
  if (length + 1 > capacity) return false;
  memcpy(dest, value, length);
  dest[length] = '\0';
  return true;
}

enum Field : uint32_t {
  kFieldT = 1u << 0,
  kFieldTurn = 1u << 1,
  kFieldFrames = 1u << 2,
  kFieldBytes = 1u << 3,
  kFieldTs = 1u << 4,
  kFieldRetry = 1u << 5,
  kFieldCode = 1u << 6,
  kFieldReason = 1u << 7,
  kFieldSession = 1u << 8,
  kFieldActivity = 1u << 9,
  kFieldFormat = 1u << 10,
};

ControlType typeFor(const char* t) {
  if (strcmp(t, "ready") == 0) return ControlType::Ready;
  if (strcmp(t, "speech_start") == 0) return ControlType::SpeechStart;
  if (strcmp(t, "turn_complete") == 0) return ControlType::TurnComplete;
  if (strcmp(t, "interrupted") == 0) return ControlType::Interrupted;
  if (strcmp(t, "error") == 0) return ControlType::Error;
  if (strcmp(t, "session_end") == 0) return ControlType::SessionEnd;
  if (strcmp(t, "pong") == 0) return ControlType::Pong;
  return ControlType::Unknown;
}

uint32_t requiredFor(ControlType type) {
  switch (type) {
    case ControlType::Ready:
      return kFieldSession | kFieldActivity | kFieldFormat;
    case ControlType::SpeechStart:
      return kFieldTurn | kFieldFormat;
    case ControlType::TurnComplete:
      return kFieldTurn | kFieldFrames | kFieldBytes;
    case ControlType::Interrupted:
      return kFieldTurn;
    case ControlType::Error:
      return kFieldCode | kFieldRetry;
    case ControlType::SessionEnd:
      return kFieldReason;
    case ControlType::Pong:
      return kFieldTs;
    case ControlType::Unknown:
      return 0;
  }
  return 0;
}

}  // namespace

ControlError parseControl(const char* text, size_t length,
                          ControlMessage& out) {
  memset(&out, 0, sizeof(out));
  out.type = ControlType::Unknown;
  if (text == nullptr) return ControlError::Malformed;
  if (length > kMaxControlBytes) return ControlError::TooLong;

  Cursor c = {text, text + length};
  skipSpace(c);
  if (c.p >= c.end || *c.p != '{') return ControlError::Malformed;
  ++c.p;

  uint32_t seen = 0;
  char typeName[kMaxString] = {0};
  bool badValue = false;

  skipSpace(c);
  if (c.p < c.end && *c.p == '}') {
    ++c.p;
  } else {
    for (;;) {
      char key[kMaxKey];
      size_t keyLength = 0;
      skipSpace(c);
      if (!readString(c, key, sizeof(key), &keyLength)) {
        return ControlError::Malformed;
      }
      skipSpace(c);
      if (c.p >= c.end || *c.p != ':') return ControlError::Malformed;
      ++c.p;
      skipSpace(c);

      // Read the value, whatever its type.
      ValueKind kind;
      char text_[kMaxString];
      size_t textLength = 0;
      uint32_t number = 0;
      bool flag = false;
      if (c.p < c.end && *c.p == '"') {
        if (!readString(c, text_, sizeof(text_), &textLength)) {
          return ControlError::Malformed;
        }
        kind = ValueKind::String;
      } else if (c.p < c.end && *c.p >= '0' && *c.p <= '9') {
        if (!readNumber(c, &number)) return ControlError::Malformed;
        kind = ValueKind::Number;
      } else if (readLiteral(c, "true")) {
        flag = true;
        kind = ValueKind::Bool;
      } else if (readLiteral(c, "false")) {
        flag = false;
        kind = ValueKind::Bool;
      } else {
        // Objects, arrays, null, negative numbers: not part of the schema.
        return ControlError::Malformed;
      }

      // Assign known keys; ignore unknown ones (forward compatibility).
      uint32_t bit = 0;
      if (strcmp(key, "t") == 0) {
        bit = kFieldT;
        if (kind != ValueKind::String) {
          badValue = true;
        } else {
          memcpy(typeName, text_, textLength + 1);
        }
      } else if (strcmp(key, "turn") == 0) {
        bit = kFieldTurn;
        if (kind != ValueKind::Number) badValue = true;
        out.turn = number;
        out.hasTurn = true;
      } else if (strcmp(key, "frames") == 0) {
        bit = kFieldFrames;
        if (kind != ValueKind::Number) badValue = true;
        out.frames = number;
      } else if (strcmp(key, "bytes") == 0) {
        bit = kFieldBytes;
        if (kind != ValueKind::Number) badValue = true;
        out.bytes = number;
      } else if (strcmp(key, "ts") == 0) {
        bit = kFieldTs;
        if (kind != ValueKind::Number) badValue = true;
        out.ts = number;
      } else if (strcmp(key, "retry") == 0) {
        bit = kFieldRetry;
        if (kind != ValueKind::Bool) badValue = true;
        out.retry = flag;
      } else if (strcmp(key, "code") == 0) {
        bit = kFieldCode;
        if (kind != ValueKind::String ||
            !copyField(text_, textLength, out.code, sizeof(out.code))) {
          badValue = true;
        }
      } else if (strcmp(key, "reason") == 0) {
        bit = kFieldReason;
        if (kind != ValueKind::String ||
            !copyField(text_, textLength, out.reason, sizeof(out.reason))) {
          badValue = true;
        }
      } else if (strcmp(key, "session") == 0) {
        bit = kFieldSession;
        if (kind != ValueKind::String ||
            !copyField(text_, textLength, out.session, sizeof(out.session))) {
          badValue = true;
        }
      } else if (strcmp(key, "activity") == 0) {
        bit = kFieldActivity;
        if (kind != ValueKind::String ||
            !copyField(text_, textLength, out.activity, sizeof(out.activity))) {
          badValue = true;
        }
      } else if (strcmp(key, "fmt") == 0 || strcmp(key, "out") == 0) {
        bit = kFieldFormat;
        if (kind != ValueKind::String ||
            !copyField(text_, textLength, out.format, sizeof(out.format))) {
          badValue = true;
        }
      }
      // A duplicated known key is ambiguous: reject rather than guess.
      if (bit != 0) {
        if ((seen & bit) != 0) return ControlError::Malformed;
        seen |= bit;
      }

      skipSpace(c);
      if (c.p < c.end && *c.p == ',') {
        ++c.p;
        continue;
      }
      if (c.p < c.end && *c.p == '}') {
        ++c.p;
        break;
      }
      return ControlError::Malformed;
    }
  }

  skipSpace(c);
  if (c.p != c.end) return ControlError::Malformed;
  if ((seen & kFieldT) == 0) return ControlError::MissingType;
  if (badValue) return ControlError::BadValue;

  out.type = typeFor(typeName);
  if (out.type == ControlType::Unknown) return ControlError::None;

  const uint32_t required = requiredFor(out.type);
  if ((seen & required) != required) return ControlError::MissingField;
  if ((required & kFieldTurn) != 0 && out.turn == kInvalidTurn) {
    return ControlError::BadValue;
  }
  if (out.hasTurn && out.turn == kInvalidTurn) return ControlError::BadValue;
  return ControlError::None;
}

}  // namespace wire
}  // namespace tth
