#pragma once

#include <stddef.h>
#include <stdint.h>

// The device side of the `tth.v1` wire protocol (docs/PHASE6_PLAN.md §2).
//
// Binary frames carry audio only, behind an 8-byte header:
//
//   0  kind      0x01 user audio (device->gateway), 0x02 model audio (gateway->device)
//   1  flags     0
//   2  reserved  0 (2 bytes)
//   4  turn_id   uint32 little-endian; 0 is RESERVED and invalid
//   8  PCM       s16le mono; 16 kHz up, 24 kHz down; even length
//
// Control frames are small JSON text messages, at most kMaxControlBytes. The
// device ENCODES device->gateway messages and PARSES gateway->device ones.
// Everything works on caller-supplied fixed buffers: no heap, no general JSON
// library. The parser accepts a flat object only (string / unsigned integer /
// boolean values) and rejects anything else as malformed.
//
// Portable: no Arduino, no ESP-IDF.

namespace tth {
namespace wire {

const uint8_t kProtocolVersion = 1;

const size_t kAudioHeaderBytes = 8;
const uint8_t kKindUserAudio = 0x01;
const uint8_t kKindModelAudio = 0x02;

// One up frame is 320 samples (20 ms at 16 kHz); one down frame at most 960
// samples (40 ms at 24 kHz) -- exactly one PcmPlayer slot.
const uint32_t kMaxUpPcmBytes = 640;
const uint32_t kMaxDownPcmBytes = 1920;

// Both sides refuse larger control frames.
const size_t kMaxControlBytes = 512;

const uint32_t kInvalidTurn = 0;

// Activity selection (on-device menu). Only ids, bounded titles and the mode
// ever reach the device -- never a prompt or participant data.
const uint32_t kMaxActivities = 8;
const size_t kActivityIdChars = 36;       // canonical lowercase UUID
const size_t kMaxActivityTitleBytes = 48;  // UTF-8

// A canonical lowercase UUID: 8-4-4-4-12 hex digits.
bool isActivityId(const char* text);

enum class FrameError : uint8_t {
  None = 0,
  TooShort,
  BadKind,
  BadFlags,
  BadReserved,
  ZeroTurn,
  EmptyPayload,
  OddPayload,
  Oversize,
};

const char* toString(FrameError error);

struct AudioFrameView {
  uint8_t kind;
  uint32_t turn;
  const uint8_t* pcm;
  uint32_t pcmBytes;
};

// Writes the 8-byte header into `out`. False (nothing meaningful written) for
// turn 0 or an unknown kind.
bool writeAudioHeader(uint8_t* out, uint8_t kind, uint32_t turn);

// Validates a complete binary frame that must be of `expectedKind`.
FrameError parseAudioFrame(const uint8_t* data, size_t length,
                           uint8_t expectedKind, AudioFrameView& out);

// --- device -> gateway (encoders) --------------------------------------------
//
// Each writes a NUL-terminated message and returns its length, or 0 if it
// would not fit `capacity` or kMaxControlBytes, or an argument is invalid
// (turn 0; a firmware string outside [A-Za-z0-9._-]{1,24}).

// `activity`: the saved on-device selection, or nullptr/"" for the gateway's
// configured default. Must be an activity id when given.
size_t encodeHello(char* out, size_t capacity, const char* firmware,
                   uint32_t credit, const char* activity = nullptr);
size_t encodeActivitySelect(char* out, size_t capacity, const char* activity);
size_t encodeTurnStart(char* out, size_t capacity, uint32_t turn);
size_t encodeTurnEnd(char* out, size_t capacity, uint32_t turn,
                     uint32_t frames, uint32_t bytes);
size_t encodeCancel(char* out, size_t capacity, uint32_t turn);
size_t encodeCredit(char* out, size_t capacity, uint32_t bytes);
size_t encodePing(char* out, size_t capacity, uint32_t ts);

// --- gateway -> device (parser) ----------------------------------------------

enum class ControlType : uint8_t {
  Unknown = 0,  // a `t` this firmware does not know: ignore and count
  Ready,
  SpeechStart,
  TurnComplete,
  Interrupted,
  Error,
  SessionEnd,
  Pong,
  // One item of the selectable list: index, count, activity, title, mode,
  // current.
  ActivityList,
  // The gateway switched to `activity` (a new Gemini session is ready).
  ActivitySelected,
  // A selection (or the saved one sent in hello) was refused: `code`, and
  // `activity` when known. The previous activity stays in use.
  ActivitySelectError,
};

const char* toString(ControlType type);

enum class ControlError : uint8_t {
  None = 0,
  TooLong,       // over kMaxControlBytes
  Malformed,     // not a flat JSON object of supported values
  MissingType,   // no "t"
  MissingField,  // a field this type requires is absent
  BadValue,      // wrong value type, turn 0, or a string too long for its field
};

const char* toString(ControlError error);

struct ControlMessage {
  ControlType type;
  uint32_t turn;
  bool hasTurn;
  uint32_t frames;
  uint32_t bytes;
  uint32_t ts;
  bool retry;
  char code[32];
  char reason[16];
  char session[40];
  char activity[40];
  char format[24];  // "fmt" or "out"
  // activity_list
  uint32_t index;
  uint32_t count;
  bool current;
  char title[kMaxActivityTitleBytes + 1];
  char mode[24];
};

ControlError parseControl(const char* text, size_t length,
                          ControlMessage& out);

}  // namespace wire
}  // namespace tth
