#pragma once

#include <stdint.h>

#include "tth/AudioFormat.h"
#include "tth/DownstreamCredit.h"
#include "tth/DownstreamRing.h"
#include "tth/GatewayProtocol.h"
#include "tth/ITurnSource.h"
#include "tth/IUplink.h"
#include "tth/TurnEventQueue.h"
#include "tth/TurnIdAllocator.h"

// The turn source that talks to the gateway (PHASE6_PLAN §3, Step 6.3).
//
// LOOP TASK ONLY. Nothing here is ever called from the network task, and
// nothing here calls App: like every ITurnSource it is polled, and its events
// and audio are pulled.
//
//   loop -> network   turn_start / audio / turn_end / cancel / credit through
//                     IUplink (the shared ordered OutboundQueue)
//   network -> loop   model audio through the DownstreamRing (SPSC; only whole
//                     published frames are visible) and control messages,
//                     which App parses and hands to onControl()
//
// A TURN
//
//   Idle ─begin─▶ Uploading ─end─▶ Ending ─turn_end queued─▶ Awaiting
//                                                              │ speech_start
//                                  Idle ◀─all bytes consumed── Completing ◀─ Responding
//                                                turn_complete─┘
//
// FIRST-RESPONSE TIMEOUT
//
// Armed when the sender reports turn_end FULLY WRITTEN (IUplink::
// lastTurnEndSent), never when it is queued. Satisfied only by the active
// turn: its first audio frame after speech_start, or a turn_complete with no
// audio. Stale events and stale audio never satisfy it. On expiry the turn
// fails exactly once (ResponseTimeout); the connection stays up.
//
// FAILURE AND CANCEL
//
// Both: queue cancel(N) (the sender purges N's unsent frames at a frame
// boundary), discard every frame of N in the ring, count the bytes as consumed
// and return the credit, finish the id, and move to a new generation so no
// queued event of N is delivered. Frames or events of N that arrive later are
// dropped and counted. A failure additionally emits one Error event; a
// failure while Idle does nothing, which is what makes it exactly once.
//
// CREDIT
//
// Returned only for bytes consumed from the ring (played into PcmPlayer, or
// discarded). A return is committed only after the credit message was
// queued, so a full outbound queue never loses credit. Frames from an older
// connection are dropped WITHOUT touching the current connection's accounting.
//
// Portable.

namespace tth {

struct GatewayTurnConfig {
  AudioFormat captureFormat;   // what user audio must be (16 kHz)
  AudioFormat responseFormat;  // what speech_start must announce (24 kHz)
  uint32_t firstResponseTimeoutMs;
  uint32_t maxUpSamples;  // largest pushUserAudio() chunk (one up frame)
};

struct GatewayTurnCounters {
  uint32_t turnsStarted;
  uint32_t turnsCompleted;
  uint32_t turnsFailed;
  uint32_t turnsCancelled;
  uint32_t beginRefused;
  uint32_t upstreamBusy;    // pushUserAudio() Busy (a retry, never a loss)
  uint32_t turnEndBusy;     // turn_end retried from poll()
  uint32_t creditReturned;  // bytes committed as returned, all connections
  uint32_t creditReturnBusy;
  uint32_t staleAudioBytes;       // frames of no active turn (current connection)
  uint32_t cancelledAudioBytes;   // frames of a cancelled or failed turn
  uint32_t oldConnectionBytes;    // frames of an older connection
  uint32_t staleEvents;
  uint32_t interrupted;
  uint32_t firstResponseTimeouts;
  uint32_t sessionLossFailures;
  uint32_t protocolFailures;
  uint32_t gatewayErrors;
  uint32_t lastFirstResponseMs;  // turn_end written -> first valid response
  uint32_t maxFirstResponseMs;
};

class GatewayTurnSource : public ITurnSource {
 public:
  enum class Phase : uint8_t {
    Idle = 0,
    Uploading,
    Ending,      // turn_end not yet queued (Busy); retried from poll()
    Awaiting,    // turn_end queued; no response yet
    Responding,  // speech_start received
    Completing,  // turn_complete received; waiting for its bytes to be consumed
  };

  static const uint32_t kScratchSamples = wire::kMaxDownPcmBytes / 2u;
  static const uint32_t kCancelledMemory = 4;

  GatewayTurnSource(IUplink& uplink, DownstreamRing& ring, DownstreamCredit& credit,
                    const GatewayTurnConfig& config);

  // --- the connection (loop) ---------------------------------------------------
  // BEFORE the connect command for `connection`: the network task is not
  // writing, so the ring is emptied, the credit epoch restarts and the ring is
  // told which connection's frames to accept. A turn still active fails.
  void onConnecting(uint32_t connection, uint32_t nowMs);
  // The session is READY: turns may start and credit may be returned.
  void onReady(uint32_t nowMs);
  // The session ended. The active turn, if any, fails once (SessionLost).
  void onSessionLost(uint32_t nowMs);
  // A turn-scoped control message (speech_start, turn_complete, interrupted,
  // error with a turn). Anything else is ignored.
  void onControl(const wire::ControlMessage& message, uint32_t nowMs);
  // The network task refused a frame over credit or ring capacity.
  void onDownstreamViolation(uint32_t nowMs);

  // --- ITurnSource ---
  bool beginUserTurn(const AudioFormat& format) override;
  PushResult pushUserAudio(const AudioChunk& chunk) override;
  void endUserTurn() override;
  void cancel() override;
  void poll(uint32_t nowMs) override;
  bool nextEvent(TurnEvent& out) override;
  bool peekPlaybackChunk(AudioChunk& out, uint32_t maxSamples) override;
  void consumePlayback(uint32_t samples) override;

  // --- diagnostics ---
  Phase phase() const { return _phase; }
  bool linkReady() const { return _linkReady; }
  uint32_t activeTurn() const { return _turn; }
  uint32_t connection() const { return _connection; }
  uint32_t generation() const { return _generation; }
  uint32_t upFrames() const { return _upFrames; }
  uint32_t upBytes() const { return _upBytes; }
  uint32_t responseBytes() const { return _responseBytes; }
  uint32_t lastResponseBytes() const { return _lastResponseBytes; }
  bool timerArmed() const { return _timerArmed; }
  const GatewayTurnCounters& counters() const { return _c; }
  uint32_t eventDrops() const { return _events.drops(); }
  uint32_t staleDiscards() const { return _events.staleDiscards(); }

 private:
  void emit(TurnEventType type, TurnError error);
  void tryPushTurnEnd();
  // The ring front, if it is audio the active response may play. Frames that
  // can never be played are discarded (and their credit returned) on the way.
  bool activeFront(RingFrame& out, bool& blocked);
  void discardFront(const RingFrame& frame);
  void purgeRing();
  void checkCompletion();
  void complete();
  void fail(TurnError error);
  void abandon();
  void satisfy(uint32_t nowMs);
  void returnCredit(bool force);
  bool wasCancelled(uint32_t turn) const;

  IUplink& _uplink;
  DownstreamRing& _ring;
  DownstreamCredit& _credit;
  const GatewayTurnConfig _config;
  TurnIdAllocator _ids;
  TurnEventQueue _events;

  Phase _phase;
  bool _linkReady;
  uint32_t _connection;
  uint32_t _generation;
  uint32_t _nowMs;

  uint32_t _turn;
  uint32_t _upFrames;
  uint32_t _upBytes;

  bool _timerArmed;
  bool _satisfied;
  uint32_t _turnEndSentMs;
  uint32_t _deadlineMs;

  uint32_t _frontOffset;  // bytes of the ring's front frame already consumed
  uint32_t _peekSamples;
  uint32_t _responseBytes;
  uint32_t _responseFrames;
  uint32_t _declaredBytes;
  uint32_t _declaredFrames;
  uint32_t _lastResponseBytes;

  uint32_t _cancelled[kCancelledMemory];
  uint32_t _cancelledHead;

  GatewayTurnCounters _c;
  int16_t _scratch[kScratchSamples];
};

const char* toString(GatewayTurnSource::Phase phase);

}  // namespace tth
