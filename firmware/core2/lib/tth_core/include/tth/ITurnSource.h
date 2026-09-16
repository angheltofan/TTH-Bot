#pragma once

#include <stdint.h>

#include "tth/AudioFormat.h"

// The seam where the AI plugs in.
//
// Phase 5 implements it with LocalMockTurnSource (loopback and synthetic).
// Phase 6 adds a gateway implementation behind exactly this interface; nothing
// in App, the player or the face may need to change when it does.
//
// NO CALLBACKS
//
// A turn source never calls into the application. It is polled, and both of
// its outputs are PULLED by the cooperative loop:
//
//   control events  nextEvent()           small value types, bounded queue
//   playback audio  peekPlaybackChunk()   a borrowed view, then
//                   consumePlayback()     an explicit advance
//
// That is deliberate. Phase 6's source will have a network task; if it could
// call back, it would be mutating App, the face renderer, AudioBus or the
// player from that task. With pull, it has no legal way to -- it can only fill
// its own preallocated storage, which the loop reads when it is ready.
//
// OWNERSHIP OF AUDIO
//
//   Outbound (pushUserAudio): the chunk is borrowed for the duration of the
//     call ONLY. A source that needs the bytes afterwards copies them before
//     returning.
//
//   Inbound (peekPlaybackChunk): the view stays valid until the next
//     NON-const call on the source (consumePlayback, cancel, poll, ...). The
//     consumer copies before then. Nothing is consumed until consumePlayback()
//     says so, which is what makes a full player ring lossless: peek, fail to
//     submit, and simply peek the same samples again next loop.

namespace tth {

// Backpressure from a turn source. Audio is never dropped on any of these.
enum class PushResult : uint8_t {
  // Taken. The caller moves on to the next samples.
  Accepted = 0,
  // Not now -- retry the SAME samples later. Nothing was taken.
  Busy,
  // The turn cannot continue. The caller aborts it with a named reason.
  Fatal,
};

const char* toString(PushResult result);

enum class TurnEventType : uint8_t {
  None = 0,
  // The response has audio. `format` is the format of every chunk that will
  // follow; the player opens a stream in exactly that format.
  SpeechStart,
  // The source has handed out its last chunk (or had no audio at all).
  TurnComplete,
  // The turn failed; `error` says why.
  Error,
};

const char* toString(TurnEventType type);

enum class TurnError : uint8_t {
  None = 0,
  // The source refused the user audio it was given.
  Rejected,
  // A diagnostic error injected on purpose (serial key), to test ERROR.
  Injected,
  // A reader could not take a lease on the recorded turn.
  BufferUnavailable,
  // --- Step 6.3: gateway turns -------------------------------------------------
  // No valid response within the first-response timeout after turn_end was
  // fully written to the socket.
  ResponseTimeout,
  // The gateway session ended while the turn was active.
  SessionLost,
  // The gateway reported an error for this turn.
  GatewayError,
  // A protocol or credit violation for this turn.
  Protocol,
};

const char* toString(TurnError error);

// Copied by value; contains no pointers, so it cannot outlive anything.
struct TurnEvent {
  TurnEventType type;
  TurnError error;
  // Meaningful for SpeechStart only.
  AudioFormat format;
  // Which turn this belongs to. cancel() moves the source to a new generation,
  // and anything stamped with an old one is discarded rather than delivered.
  uint32_t generation;
};

class ITurnSource {
 public:
  virtual ~ITurnSource() {}

  // A user turn begins. Returns false if the source cannot take one now (for
  // example, the previous response was never cancelled).
  virtual bool beginUserTurn(const AudioFormat& format) = 0;

  // One piece of the user's speech, in the format given to beginUserTurn().
  // Borrowed for this call only. MUST NOT block.
  virtual PushResult pushUserAudio(const AudioChunk& chunk) = 0;

  // Every sample of the user's turn has been pushed.
  virtual void endUserTurn() = 0;

  // Abandons the current turn, whatever phase it is in. After this returns no
  // event or chunk from the abandoned turn may be delivered.
  virtual void cancel() = 0;

  // Called once per loop. MUST NOT block.
  virtual void poll(uint32_t nowMs) = 0;

  // Takes the next control event, if any. Returns false when there is none.
  virtual bool nextEvent(TurnEvent& out) = 0;

  // A view of up to `maxSamples` of response audio, not yet consumed.
  // Returns false when nothing is available right now.
  virtual bool peekPlaybackChunk(AudioChunk& out, uint32_t maxSamples) = 0;

  // Marks `samples` of the most recent peek as consumed.
  virtual void consumePlayback(uint32_t samples) = 0;
};

}  // namespace tth
