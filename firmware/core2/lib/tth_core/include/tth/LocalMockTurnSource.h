#pragma once

#include <stdint.h>

#include "tth/AudioFormat.h"
#include "tth/ITurnSource.h"
#include "tth/PcmGain.h"
#include "tth/TurnBuffer.h"
#include "tth/TurnEventQueue.h"

// An offline stand-in for the AI, behind the real ITurnSource interface.
//
// Two modes, latched per user turn:
//
//   LOOPBACK   replays the child's own recording at its NATIVE 16 kHz, read
//              out of the PSRAM TurnBuffer under a lease. At 0 dB it is handed
//              out zero-copy, bit for bit; with loopback gain it is amplified
//              into this source's own scratch chunk (see PcmGain.h).
//
//   SYNTHETIC  a deterministic 24 kHz voice-like tone shaped by the same
//              quiet / medium / loud / silence envelope the face diagnostics
//              use. NEVER affected by the loopback gain.
//
// OWNERSHIP OF THE USER'S AUDIO
//
// pushUserAudio() receives a BORROWED pointer, valid for that call only, and
// keeps nothing of it -- not the pointer, not the chunk. It records a count
// (_received) and, for loopback, reads the samples DURING the call into a
// level histogram used to choose the turn's gain.
//
// Loopback replays those samples later, so it must know they will still be
// there. It gets that from its OWN lease on the TurnBuffer it was constructed
// with:
//
//   beginUserTurn()   takes the lease (loopback only).
//   pushUserAudio()   accepts a chunk ONLY if it is exactly the next committed
//                     range of that leased buffer:
//                       chunk.samples == data() + _received, and
//                       _received + count <= committedSamples().
//                     Anything else -- an identical copy at another address, a
//                     gap, a repeat, uncommitted samples -- is Fatal.
//   peekPlaybackChunk() reads from the leased buffer, data() + cursor --
//                     directly at 0 dB, or through the gain into scratch.
//                     Never from a pushed pointer.
//   last consume      PcmPlayer has copied the final chunk into its own slots;
//                     only then is the lease released.
//   cancel()          releases it, in any phase.
//
// The TurnStreamer's lease and this one OVERLAP, so there is no instant
// between recording and replay when nothing holds the buffer.
//
// Diagnostics for the physical test, off by default:
//   backpressure  pushUserAudio() returns Busy in regular windows.
//   inject error  the next response is an Error event instead of speech.

namespace tth {

enum class MockMode : uint8_t {
  Loopback = 0,
  Synthetic,
};

const char* toString(MockMode mode);

struct MockConfig {
  AudioFormat captureFormat;    // what the user audio must be
  AudioFormat assistantFormat;  // what synthetic speech is generated in
  uint32_t thinkMs;             // WAITING before the response starts
  uint32_t synthDurationMs;
  // Simulated backpressure: Busy for `busyWindowMs` out of every
  // `busyPeriodMs`.
  uint32_t busyWindowMs;
  uint32_t busyPeriodMs;
  // Digital gain for LOOPBACK replay only. 0 = exact bypass.
  float loopbackGainDb = 0.0f;
};

class LocalMockTurnSource : public ITurnSource {
 public:
  enum class Phase : uint8_t {
    Idle = 0,
    Receiving,   // user turn in progress
    Thinking,    // user turn ended, response not started
    Responding,  // handing out response audio
  };

  LocalMockTurnSource(TurnBuffer& turn, const MockConfig& config);

  // One chunk of scratch, used for synthetic speech and for gained loopback.
  // Without it those report an error instead of speaking.
  void attachSynthScratch(int16_t* scratch, uint32_t samples);

  // Applies from the next user turn: the mode is latched by beginUserTurn(),
  // so a turn never changes which lease rules apply to it half-way through.
  void setMode(MockMode mode) { _mode = mode; }
  MockMode mode() const { return _mode; }
  // The mode of the turn in progress (or the last one).
  MockMode turnMode() const { return _turnMode; }

  // Loopback gain. Applies from the next user turn (latched with the mode).
  void setLoopbackGainDb(float db);
  float loopbackGainDb() const { return gainQ12ToDb(_loopbackGainQ12); }

  void setBackpressure(bool on) { _backpressure = on; }
  bool backpressure() const { return _backpressure; }

  void injectErrorOnNextTurn() { _injectError = true; }
  bool errorPending() const { return _injectError; }

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
  uint32_t receivedSamples() const { return _received; }
  // Chunks refused because they were not exactly the next committed range of
  // the turn buffer (a gap, a repeat, a copy elsewhere, uncommitted samples).
  uint32_t continuityErrors() const { return _continuityErrors; }
  uint32_t busyReturned() const { return _busyReturned; }
  uint32_t responseSamples() const { return _responseTotal; }
  uint32_t responseCursor() const { return _responseCursor; }
  const AudioFormat& responseFormat() const { return _responseFormat; }
  uint32_t generation() const { return _generation; }
  uint32_t eventDrops() const { return _events.drops(); }
  uint32_t staleDiscards() const { return _events.staleDiscards(); }
  bool holdsLease() const { return _leaseHeld; }

  // --- loudness of the current (or last) response ---
  // Gain configured for the turn, and the gain actually applied after fitting
  // it to the turn's level. Both 0 dB for synthetic.
  float configuredTurnGainDb() const {
    return gainQ12ToDb(_turnConfiguredGainQ12);
  }
  float appliedGainDb() const { return gainQ12ToDb(_turnGainQ12); }
  int32_t appliedGainQ12() const { return _turnGainQ12; }
  // Levels of the user turn, from the histogram: absolute peak, and the
  // 99.9th-percentile level the gain was fitted to. Full scale = 32767.
  int32_t turnInputPeak() const { return _levels.peak(); }
  int32_t turnRobustPeak() const { return _robustPeak; }
  // Over the samples actually handed out (consumed) so far.
  int32_t responseInputPeak() const { return _responseInputPeak; }
  int32_t responseOutputPeak() const { return _responseOutputPeak; }
  uint32_t limitedSamples() const { return _limitedSamples; }

 private:
  void emit(TurnEventType type, TurnError error, const AudioFormat& format);
  void respond();
  void finishResponse();
  void releaseLease();
  void accountConsumed(uint32_t samples);
  bool inBusyWindow() const;
  void generateSynth(uint32_t position, uint32_t count);

  TurnBuffer& _turn;
  const MockConfig _config;
  TurnEventQueue _events;

  MockMode _mode;
  MockMode _turnMode;
  Phase _phase;
  bool _backpressure;
  bool _injectError;
  bool _leaseHeld;
  uint32_t _generation;
  uint32_t _nowMs;
  uint32_t _thinkUntilMs;

  // The ONLY thing kept from pushUserAudio(): a count (plus the histogram).
  uint32_t _received;
  uint32_t _continuityErrors;
  uint32_t _busyReturned;

  AudioFormat _responseFormat;
  uint32_t _responseTotal;
  uint32_t _responseCursor;

  int32_t _loopbackGainQ12;        // setting, for the next turn
  int32_t _turnConfiguredGainQ12;  // latched at beginUserTurn()
  int32_t _turnGainQ12;            // fitted at respond(); unity for synthetic
  TurnLevelHistogram _levels;
  int32_t _robustPeak;
  int32_t _responseInputPeak;
  int32_t _responseOutputPeak;
  uint32_t _limitedSamples;

  int16_t* _scratch;
  uint32_t _scratchSamples;
  uint32_t _scratchPos;
  uint32_t _scratchCount;

  static const uint32_t kSineSize = 256;
  int16_t _sine[kSineSize];
};

}  // namespace tth
