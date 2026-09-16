#include "tth/LocalMockTurnSource.h"

#include <math.h>

#include "tth/FaceAnimator.h"

namespace tth {

namespace {

const float kTwoPi = 6.28318530718f;

// A low, calm fundamental with two harmonics reads as "voice-like" through the
// Core2's small speaker far better than a pure sine does.
const uint32_t kSynthFundamentalHz = 180;
// Headroom: the envelope peaks at 1.0, and the speaker distorts near full
// scale.
const float kSynthGain = 0.55f;
// The envelope is evaluated once per block rather than per sample; 2 ms at
// 24 kHz is far finer than anything the bars or the ear can resolve.
const uint32_t kEnvelopeBlock = 48;

// The loopback gain is fitted so that 99.9 % of the turn stays below the
// limiter knee; the loudest 0.1 % (clicks, plosives) is soft-limited rather
// than being allowed to pull the whole turn's gain down.
const uint32_t kRobustPermille = 999;

int32_t magnitudeOf(int16_t sample) {
  const int32_t v = sample;
  return (v < 0) ? -v : v;
}

}  // namespace

const char* toString(MockMode mode) {
  switch (mode) {
    case MockMode::Loopback:
      return "loopback";
    case MockMode::Synthetic:
      return "synthetic";
  }
  return "invalid";
}

LocalMockTurnSource::LocalMockTurnSource(TurnBuffer& turn,
                                         const MockConfig& config)
    : _turn(turn),
      _config(config),
      _mode(MockMode::Loopback),
      _turnMode(MockMode::Loopback),
      _phase(Phase::Idle),
      _backpressure(false),
      _injectError(false),
      _leaseHeld(false),
      _generation(1),
      _nowMs(0),
      _thinkUntilMs(0),
      _received(0),
      _continuityErrors(0),
      _busyReturned(0),
      _responseFormat(monoS16(0)),
      _responseTotal(0),
      _responseCursor(0),
      _loopbackGainQ12(dbToGainQ12(config.loopbackGainDb)),
      _turnConfiguredGainQ12(kGainUnityQ12),
      _turnGainQ12(kGainUnityQ12),
      _robustPeak(0),
      _responseInputPeak(0),
      _responseOutputPeak(0),
      _limitedSamples(0),
      _scratch(nullptr),
      _scratchSamples(0),
      _scratchPos(0xFFFFFFFFu),
      _scratchCount(0) {
  for (uint32_t i = 0; i < kSineSize; ++i) {
    const float angle = kTwoPi * static_cast<float>(i) /
                        static_cast<float>(kSineSize);
    _sine[i] = static_cast<int16_t>(32767.0f * sinf(angle));
  }
}

void LocalMockTurnSource::attachSynthScratch(int16_t* scratch,
                                             uint32_t samples) {
  _scratch = scratch;
  _scratchSamples = (scratch == nullptr) ? 0 : samples;
  _scratchPos = 0xFFFFFFFFu;
  _scratchCount = 0;
}

void LocalMockTurnSource::setLoopbackGainDb(float db) {
  _loopbackGainQ12 = dbToGainQ12(db);
}

void LocalMockTurnSource::emit(TurnEventType type, TurnError error,
                               const AudioFormat& format) {
  TurnEvent event;
  event.type = type;
  event.error = error;
  event.format = format;
  event.generation = _generation;
  _events.push(event);
}

void LocalMockTurnSource::releaseLease() {
  if (!_leaseHeld) return;
  _turn.release();
  _leaseHeld = false;
}

bool LocalMockTurnSource::beginUserTurn(const AudioFormat& format) {
  // A previous response must have finished or been cancelled first.
  if (_phase != Phase::Idle) return false;
  if (!sameFormat(format, _config.captureFormat)) return false;

  _turnMode = _mode;
  if (_turnMode == MockMode::Loopback) {
    // Loopback will replay what it accepts, so it depends on these samples
    // from the first one onwards. The lease is taken NOW -- while the
    // streamer still holds its own -- and kept until the last sample has been
    // copied out, or the turn is cancelled.
    if (!_turn.retain()) return false;
    _leaseHeld = true;
    _turnConfiguredGainQ12 = _loopbackGainQ12;
  } else {
    // Synthetic speech is never touched by the loopback gain.
    _turnConfiguredGainQ12 = kGainUnityQ12;
  }

  _levels.reset();
  _received = 0;
  _continuityErrors = 0;
  _busyReturned = 0;
  _phase = Phase::Receiving;
  return true;
}

bool LocalMockTurnSource::inBusyWindow() const {
  if (_config.busyPeriodMs == 0) return false;
  return (_nowMs % _config.busyPeriodMs) < _config.busyWindowMs;
}

PushResult LocalMockTurnSource::pushUserAudio(const AudioChunk& chunk) {
  if (_phase != Phase::Receiving) return PushResult::Fatal;
  if (chunk.samples == nullptr ||
      !sameFormat(chunk.format, _config.captureFormat)) {
    return PushResult::Fatal;
  }

  // Integrity before backpressure: a malformed chunk is fatal even inside a
  // busy window.
  //
  // Accept ONLY the next committed range of the buffer this source reads from.
  // That is what lets it keep nothing of the borrowed chunk: everything it
  // accepts is already covered by its own lease, at an address it can
  // re-derive from the buffer itself.
  const bool contiguous = (chunk.samples == _turn.data() + _received);
  const bool committed = (static_cast<uint64_t>(_received) + chunk.count <=
                          _turn.committedSamples());
  if (!contiguous || !committed) {
    ++_continuityErrors;
    return PushResult::Fatal;
  }

  if (_backpressure && inBusyWindow()) {
    ++_busyReturned;
    return PushResult::Busy;
  }

  // Read DURING the call only, to choose this turn's gain. Counted once per
  // accepted sample -- a Busy retry adds nothing.
  if (_turnMode == MockMode::Loopback) _levels.add(chunk.samples, chunk.count);

  // A count. The pointer is not stored; it is not needed.
  _received += chunk.count;
  return PushResult::Accepted;
}

void LocalMockTurnSource::endUserTurn() {
  if (_phase != Phase::Receiving) return;
  _phase = Phase::Thinking;
  _thinkUntilMs = _nowMs + _config.thinkMs;
}

void LocalMockTurnSource::cancel() {
  // A new generation: anything already queued for the old turn is now stale
  // and will be discarded rather than delivered. The loudness figures of the
  // response are kept, so a barge-in's playback summary can still report them.
  ++_generation;
  releaseLease();
  _phase = Phase::Idle;
  _responseTotal = 0;
  _responseCursor = 0;
}

void LocalMockTurnSource::poll(uint32_t nowMs) {
  _nowMs = nowMs;
  if (_phase == Phase::Thinking &&
      static_cast<int32_t>(nowMs - _thinkUntilMs) >= 0) {
    respond();
  }
}

void LocalMockTurnSource::respond() {
  _responseCursor = 0;
  _responseInputPeak = 0;
  _responseOutputPeak = 0;
  _limitedSamples = 0;
  _robustPeak = 0;
  _turnGainQ12 = kGainUnityQ12;
  _scratchPos = 0xFFFFFFFFu;
  _scratchCount = 0;

  if (_injectError) {
    _injectError = false;
    releaseLease();
    _phase = Phase::Idle;
    emit(TurnEventType::Error, TurnError::Injected, _config.captureFormat);
    return;
  }

  if (_turnMode == MockMode::Loopback) {
    if (!_leaseHeld) {
      // Unreachable: beginUserTurn() refuses a loopback turn without a lease.
      _phase = Phase::Idle;
      emit(TurnEventType::Error, TurnError::BufferUnavailable,
           _config.captureFormat);
      return;
    }
    if (_received == 0) {
      // Nothing was said: complete quietly, with no speech.
      releaseLease();
      _phase = Phase::Idle;
      emit(TurnEventType::TurnComplete, TurnError::None,
           _config.captureFormat);
      return;
    }

    // ONE gain for the whole turn, from the whole turn's level. Never chunk
    // by chunk, so quiet passages between words are not pumped up and down.
    _robustPeak = _levels.levelCovering(kRobustPermille);
    _turnGainQ12 = fitTurnGainQ12(_turnConfiguredGainQ12, _robustPeak);
    if (_turnGainQ12 != kGainUnityQ12 &&
        (_scratch == nullptr || _scratchSamples == 0)) {
      releaseLease();
      _phase = Phase::Idle;
      emit(TurnEventType::Error, TurnError::BufferUnavailable,
           _config.captureFormat);
      return;
    }

    // Exactly what was accepted -- not whatever the buffer happens to hold.
    _responseTotal = _received;
    // NATIVE rate: the recording is 16 kHz, so it is played as 16 kHz.
    _responseFormat = _config.captureFormat;
  } else {
    if (_scratch == nullptr || _scratchSamples == 0) {
      _phase = Phase::Idle;
      emit(TurnEventType::Error, TurnError::BufferUnavailable,
           _config.assistantFormat);
      return;
    }
    _responseTotal = static_cast<uint32_t>(
        (static_cast<uint64_t>(_config.assistantFormat.sampleRate) *
         _config.synthDurationMs) /
        1000u);
    _responseFormat = _config.assistantFormat;
  }

  _phase = Phase::Responding;
  emit(TurnEventType::SpeechStart, TurnError::None, _responseFormat);
}

bool LocalMockTurnSource::peekPlaybackChunk(AudioChunk& out,
                                            uint32_t maxSamples) {
  if (_phase != Phase::Responding || maxSamples == 0) return false;
  const uint32_t remaining = _responseTotal - _responseCursor;
  if (remaining == 0) return false;

  uint32_t count = (remaining < maxSamples) ? remaining : maxSamples;

  if (_turnMode == MockMode::Loopback) {
    // Read from the buffer this source LEASES -- never from a pointer it was
    // once handed. The lease guarantees these samples are still there.
    if (!_leaseHeld) return false;
    const int16_t* source = _turn.data() + _responseCursor;
    if (_turnGainQ12 == kGainUnityQ12) {
      // 0 dB: the recording itself, zero copy, bit for bit.
      out.samples = source;
    } else {
      if (count > _scratchSamples) count = _scratchSamples;
      // Processed once per position; peeking again after a full player ring
      // returns the same samples. The gain is a pure per-sample function, so
      // the result does not depend on where chunk boundaries fall.
      if (_scratchPos != _responseCursor || _scratchCount < count) {
        for (uint32_t i = 0; i < count; ++i) {
          _scratch[i] = applyGain(source[i], _turnGainQ12, nullptr);
        }
        _scratchPos = _responseCursor;
        _scratchCount = count;
      }
      out.samples = _scratch;
    }
  } else {
    if (count > _scratchSamples) count = _scratchSamples;
    // Regenerated only when the position moved, so peeking the same samples
    // again after a full player ring returns identical audio.
    if (_scratchPos != _responseCursor || _scratchCount < count) {
      generateSynth(_responseCursor, count);
      _scratchPos = _responseCursor;
      _scratchCount = count;
    }
    out.samples = _scratch;
  }

  if (count == 0) return false;
  out.count = count;
  out.format = _responseFormat;
  return true;
}

void LocalMockTurnSource::accountConsumed(uint32_t samples) {
  // Loudness figures over exactly the samples handed out, each counted once --
  // a re-peek after a full player ring is not counted again.
  if (_turnMode == MockMode::Loopback) {
    if (!_leaseHeld) return;
    const int16_t* source = _turn.data() + _responseCursor;
    for (uint32_t i = 0; i < samples; ++i) {
      bool limited = false;
      const int16_t out = applyGain(source[i], _turnGainQ12, &limited);
      const int32_t in = magnitudeOf(source[i]);
      const int32_t level = magnitudeOf(out);
      if (in > _responseInputPeak) _responseInputPeak = in;
      if (level > _responseOutputPeak) _responseOutputPeak = level;
      if (limited) ++_limitedSamples;
    }
    return;
  }

  // Synthetic: no gain, so in and out are the same samples.
  if (_scratchPos != _responseCursor || _scratchCount < samples) return;
  for (uint32_t i = 0; i < samples; ++i) {
    const int32_t level = magnitudeOf(_scratch[i]);
    if (level > _responseInputPeak) _responseInputPeak = level;
    if (level > _responseOutputPeak) _responseOutputPeak = level;
  }
}

void LocalMockTurnSource::consumePlayback(uint32_t samples) {
  if (_phase != Phase::Responding) return;
  const uint32_t remaining = _responseTotal - _responseCursor;
  const uint32_t taken = (samples < remaining) ? samples : remaining;
  accountConsumed(taken);
  _responseCursor += taken;
  if (_responseCursor == _responseTotal) finishResponse();
}

void LocalMockTurnSource::finishResponse() {
  // The consumer has copied the last chunk out (PcmPlayer copies into its own
  // slots before consumePlayback() is called), so nothing depends on the turn
  // buffer any more.
  releaseLease();
  _phase = Phase::Idle;
  emit(TurnEventType::TurnComplete, TurnError::None, _responseFormat);
}

bool LocalMockTurnSource::nextEvent(TurnEvent& out) {
  return _events.pop(out, _generation);
}

void LocalMockTurnSource::generateSynth(uint32_t position, uint32_t count) {
  const uint32_t rate = _config.assistantFormat.sampleRate;
  if (rate == 0) return;

  // Phase increments for the fundamental and its 2nd and 3rd harmonics, as
  // 32-bit fixed point turns. Computed from the ABSOLUTE sample position, so a
  // given position always produces the same sample.
  const uint32_t step1 = static_cast<uint32_t>(
      (static_cast<uint64_t>(kSynthFundamentalHz) << 32) / rate);
  const uint32_t step2 = step1 * 2u;
  const uint32_t step3 = step1 * 3u;

  float envelope = 0.0f;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t p = position + i;
    if (i == 0 || (p % kEnvelopeBlock) == 0) {
      const uint32_t ms =
          static_cast<uint32_t>((static_cast<uint64_t>(p) * 1000u) / rate);
      envelope = simulatedSpeechAmplitude(ms) * kSynthGain;
    }

    const uint32_t ph1 = static_cast<uint32_t>(p * step1);
    const uint32_t ph2 = static_cast<uint32_t>(p * step2);
    const uint32_t ph3 = static_cast<uint32_t>(p * step3);
    const int32_t mixed = (4 * static_cast<int32_t>(_sine[ph1 >> 24]) +
                           2 * static_cast<int32_t>(_sine[ph2 >> 24]) +
                           static_cast<int32_t>(_sine[ph3 >> 24])) /
                          7;
    _scratch[i] = static_cast<int16_t>(static_cast<float>(mixed) * envelope);
  }
}

}  // namespace tth
