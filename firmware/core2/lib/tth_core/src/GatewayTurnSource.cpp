#include "tth/GatewayTurnSource.h"

#include <stdio.h>
#include <string.h>

// The ring stores the gateway's s16le bytes as they arrived; the scratch is
// filled by a byte copy. Every target this firmware builds for (ESP32, x86
// hosts) is little-endian.
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "GatewayTurnSource copies s16le PCM bytewise and needs a little-endian target"
#endif

namespace tth {

namespace {

enum class Front : uint8_t {
  None = 0,  // nothing the active response can use
  Playable,  // audio of the active turn, response started
  Waiting,   // audio of the active turn before speech_start was handled
  Early,     // audio of the active turn before turn_end: a protocol violation
};

}  // namespace

const char* toString(GatewayTurnSource::Phase phase) {
  switch (phase) {
    case GatewayTurnSource::Phase::Idle:
      return "idle";
    case GatewayTurnSource::Phase::Uploading:
      return "uploading";
    case GatewayTurnSource::Phase::Ending:
      return "ending";
    case GatewayTurnSource::Phase::Awaiting:
      return "awaiting";
    case GatewayTurnSource::Phase::Responding:
      return "responding";
    case GatewayTurnSource::Phase::Completing:
      return "completing";
  }
  return "invalid";
}

GatewayTurnSource::GatewayTurnSource(IUplink& uplink, DownstreamRing& ring,
                                     DownstreamCredit& credit,
                                     const GatewayTurnConfig& config)
    : _uplink(uplink),
      _ring(ring),
      _credit(credit),
      _config(config),
      _phase(Phase::Idle),
      _linkReady(false),
      _connection(0),
      _generation(0),
      _nowMs(0),
      _turn(0),
      _upFrames(0),
      _upBytes(0),
      _timerArmed(false),
      _satisfied(false),
      _turnEndSentMs(0),
      _deadlineMs(0),
      _frontOffset(0),
      _peekSamples(0),
      _responseBytes(0),
      _responseFrames(0),
      _declaredBytes(0),
      _declaredFrames(0),
      _lastResponseBytes(0),
      _cancelledHead(0) {
  for (uint32_t i = 0; i < kCancelledMemory; ++i) _cancelled[i] = 0;
  memset(&_c, 0, sizeof(_c));
  memset(_scratch, 0, sizeof(_scratch));
}

// --- the connection ---------------------------------------------------------------

void GatewayTurnSource::onConnecting(uint32_t connection, uint32_t nowMs) {
  _nowMs = nowMs;
  _linkReady = false;
  if (_phase != Phase::Idle) {
    ++_c.sessionLossFailures;
    fail(TurnError::SessionLost);
  }
  // The network task is not writing (no connection is open), so everything
  // left in the ring belongs to a connection that is gone. Its credit died
  // with that connection: dropped without touching the new epoch.
  RingFrame frame;
  while (_ring.front(frame)) {
    const uint32_t rest = frame.pcmBytes > _frontOffset ? frame.pcmBytes - _frontOffset : 0;
    _frontOffset = 0;
    _c.oldConnectionBytes += rest;
    _ring.popFront();
  }
  _frontOffset = 0;
  _credit.beginConsumerEpoch();
  _ring.acceptConnection(connection);
  _connection = connection;
}

void GatewayTurnSource::onReady(uint32_t nowMs) {
  _nowMs = nowMs;
  _linkReady = true;
}

void GatewayTurnSource::onSessionLost(uint32_t nowMs) {
  _nowMs = nowMs;
  _linkReady = false;
  if (_phase == Phase::Idle) return;
  ++_c.sessionLossFailures;
  fail(TurnError::SessionLost);
}

void GatewayTurnSource::onDownstreamViolation(uint32_t nowMs) {
  _nowMs = nowMs;
  if (_phase == Phase::Idle) return;
  ++_c.protocolFailures;
  fail(TurnError::Protocol);
}

void GatewayTurnSource::onControl(const wire::ControlMessage& m, uint32_t nowMs) {
  _nowMs = nowMs;
  switch (m.type) {
    case wire::ControlType::SpeechStart:
    case wire::ControlType::TurnComplete:
    case wire::ControlType::Interrupted:
    case wire::ControlType::Error:
      break;
    default:
      return;
  }
  // A connection-scoped error belongs to the session, not to a turn.
  if (!m.hasTurn) return;
  if (_phase == Phase::Idle || m.turn != _turn) {
    ++_c.staleEvents;
    return;
  }

  switch (m.type) {
    case wire::ControlType::SpeechStart: {
      char expected[sizeof(m.format)];
      snprintf(expected, sizeof(expected), "s16le/%lu/%u",
               static_cast<unsigned long>(_config.responseFormat.sampleRate),
               static_cast<unsigned>(_config.responseFormat.channels));
      if (_phase != Phase::Awaiting || strcmp(m.format, expected) != 0) {
        ++_c.protocolFailures;
        fail(TurnError::Protocol);
        return;
      }
      _phase = Phase::Responding;
      emit(TurnEventType::SpeechStart, TurnError::None);
      return;
    }

    case wire::ControlType::TurnComplete:
      if (_phase == Phase::Awaiting) {
        if (m.bytes != 0 || m.frames != 0) {
          // Audio was announced without speech_start.
          ++_c.protocolFailures;
          fail(TurnError::Protocol);
          return;
        }
        satisfy(nowMs);
        complete();
        return;
      }
      if (_phase != Phase::Responding) {
        ++_c.protocolFailures;
        fail(TurnError::Protocol);
        return;
      }
      _declaredBytes = m.bytes;
      _declaredFrames = m.frames;
      _phase = Phase::Completing;
      checkCompletion();
      return;

    case wire::ControlType::Interrupted:
      // Gemini stopped generating; the gateway still closes the response with
      // turn_complete, which is what ends the turn here. Counted only.
      ++_c.interrupted;
      return;

    case wire::ControlType::Error:
      ++_c.gatewayErrors;
      fail(TurnError::GatewayError);
      return;

    default:
      return;
  }
}

// --- ITurnSource ----------------------------------------------------------------------

bool GatewayTurnSource::beginUserTurn(const AudioFormat& format) {
  if (!_linkReady || _phase != Phase::Idle || !sameFormat(format, _config.captureFormat)) {
    ++_c.beginRefused;
    return false;
  }
  const uint32_t turn = _ids.allocate();
  if (_uplink.pushTurnStart(turn) != OutPush::Accepted) {
    _ids.finish(turn);
    ++_c.beginRefused;
    return false;
  }
  ++_generation;
  _events.clear();
  _turn = turn;
  _phase = Phase::Uploading;
  _upFrames = 0;
  _upBytes = 0;
  _timerArmed = false;
  _satisfied = false;
  _peekSamples = 0;
  _responseBytes = 0;
  _responseFrames = 0;
  _declaredBytes = 0;
  _declaredFrames = 0;
  ++_c.turnsStarted;
  return true;
}

PushResult GatewayTurnSource::pushUserAudio(const AudioChunk& chunk) {
  if (_phase != Phase::Uploading || chunk.samples == nullptr || chunk.count == 0 ||
      chunk.count > _config.maxUpSamples || !sameFormat(chunk.format, _config.captureFormat)) {
    return PushResult::Fatal;
  }
  switch (_uplink.pushAudio(_turn, chunk.samples, chunk.count)) {
    case OutPush::Accepted:
      ++_upFrames;
      _upBytes += chunk.count * 2u;
      return PushResult::Accepted;
    case OutPush::Busy:
      // Nothing taken: the streamer offers the same samples again.
      ++_c.upstreamBusy;
      return PushResult::Busy;
    case OutPush::Invalid:
      return PushResult::Fatal;
  }
  return PushResult::Fatal;
}

void GatewayTurnSource::endUserTurn() {
  if (_phase != Phase::Uploading) return;
  _phase = Phase::Ending;
  tryPushTurnEnd();
}

void GatewayTurnSource::tryPushTurnEnd() {
  switch (_uplink.pushTurnEnd(_turn, _upFrames, _upBytes)) {
    case OutPush::Accepted:
      _phase = Phase::Awaiting;
      return;
    case OutPush::Busy:
      ++_c.turnEndBusy;
      return;
    case OutPush::Invalid:
      ++_c.protocolFailures;
      fail(TurnError::Protocol);
      return;
  }
}

void GatewayTurnSource::cancel() {
  if (_phase != Phase::Idle) {
    abandon();
    ++_c.turnsCancelled;
  } else {
    ++_generation;
    _events.clear();
  }
  returnCredit(true);
}

void GatewayTurnSource::poll(uint32_t nowMs) {
  _nowMs = nowMs;
  if (_phase == Phase::Ending) tryPushTurnEnd();

  RingFrame frame;
  bool blocked = false;
  const bool playable = activeFront(frame, blocked);
  if (_phase == Phase::Idle) {
    // activeFront() already discarded whatever was stale.
  } else if (playable) {
    if (!_satisfied) satisfy(nowMs);
  }

  if ((_phase == Phase::Awaiting || _phase == Phase::Responding) && !_satisfied) {
    if (!_timerArmed && _uplink.lastTurnEndSent() == _turn) {
      _timerArmed = true;
      _turnEndSentMs = nowMs;
      _deadlineMs = nowMs + _config.firstResponseTimeoutMs;
    }
    if (_timerArmed && static_cast<int32_t>(nowMs - _deadlineMs) >= 0) {
      ++_c.firstResponseTimeouts;
      fail(TurnError::ResponseTimeout);
      return;
    }
  }

  if (_phase == Phase::Completing) checkCompletion();
  returnCredit(_phase != Phase::Responding);
}

bool GatewayTurnSource::nextEvent(TurnEvent& out) { return _events.pop(out, _generation); }

bool GatewayTurnSource::peekPlaybackChunk(AudioChunk& out, uint32_t maxSamples) {
  _peekSamples = 0;
  if (_phase != Phase::Responding && _phase != Phase::Completing) return false;
  RingFrame frame;
  bool blocked = false;
  if (!activeFront(frame, blocked)) return false;
  if (!_satisfied) satisfy(_nowMs);

  const uint32_t samples = maxSamples < kScratchSamples ? maxSamples : kScratchSamples;
  const uint32_t bytes =
      _ring.copyPcm(_frontOffset, reinterpret_cast<uint8_t*>(_scratch), samples * 2u) & ~1u;
  if (bytes == 0) return false;
  out.samples = _scratch;
  out.count = bytes / 2u;
  out.format = _config.responseFormat;
  _peekSamples = out.count;
  return true;
}

void GatewayTurnSource::consumePlayback(uint32_t samples) {
  if (samples > _peekSamples) samples = _peekSamples;
  _peekSamples = 0;
  if (samples == 0) return;

  const uint32_t bytes = samples * 2u;
  _credit.consumed(bytes);
  _frontOffset += bytes;
  _responseBytes += bytes;
  RingFrame frame;
  if (_ring.front(frame) && _frontOffset >= frame.pcmBytes) {
    _ring.popFront();
    _frontOffset = 0;
    ++_responseFrames;
  }
  if (_phase == Phase::Completing) checkCompletion();
  returnCredit(_phase != Phase::Responding);
}

// --- internals ------------------------------------------------------------------------

void GatewayTurnSource::emit(TurnEventType type, TurnError error) {
  TurnEvent event;
  event.type = type;
  event.error = error;
  event.format = _config.responseFormat;
  event.generation = _generation;
  _events.push(event);
}

bool GatewayTurnSource::activeFront(RingFrame& frame, bool& blocked) {
  blocked = false;
  while (_ring.front(frame)) {
    if (frame.connection == _connection && _turn != 0 && frame.turn == _turn) {
      if (_phase == Phase::Responding || _phase == Phase::Completing) return true;
      if (_phase == Phase::Awaiting) {
        // speech_start is already queued behind this audio (the network task
        // posts it first); leave the frame for it.
        blocked = true;
        return false;
      }
      // Uploading or Ending: the gateway answered a turn it has not heard
      // the end of.
      ++_c.protocolFailures;
      fail(TurnError::Protocol);
      return false;
    }
    discardFront(frame);
  }
  return false;
}

void GatewayTurnSource::discardFront(const RingFrame& frame) {
  const uint32_t rest = frame.pcmBytes > _frontOffset ? frame.pcmBytes - _frontOffset : 0;
  _frontOffset = 0;
  if (frame.connection == _connection) {
    // Consumed without playing: its credit goes back like played audio's.
    _credit.consumed(rest);
    if (wasCancelled(frame.turn)) {
      _c.cancelledAudioBytes += rest;
    } else {
      _c.staleAudioBytes += rest;
    }
  } else {
    _c.oldConnectionBytes += rest;
  }
  _ring.popFront();
}

void GatewayTurnSource::purgeRing() {
  RingFrame frame;
  while (_ring.front(frame)) discardFront(frame);
}

void GatewayTurnSource::checkCompletion() {
  if (_responseBytes > _declaredBytes || _responseFrames > _declaredFrames) {
    ++_c.protocolFailures;
    fail(TurnError::Protocol);
    return;
  }
  // Every frame of the turn was published before its turn_complete was
  // queued, so what is visible now is everything that will ever arrive.
  RingFrame frame;
  bool blocked = false;
  const bool more = activeFront(frame, blocked);
  if (_phase != Phase::Completing) return;
  if (_responseBytes == _declaredBytes) {
    if (_responseFrames != _declaredFrames || more) {
      // Declared less than was delivered.
      ++_c.protocolFailures;
      fail(TurnError::Protocol);
      return;
    }
    complete();
    return;
  }
  // Bytes are still owed and none are left to play: declared more than was
  // delivered.
  if (!more) {
    ++_c.protocolFailures;
    fail(TurnError::Protocol);
  }
}

void GatewayTurnSource::complete() {
  ++_c.turnsCompleted;
  _ids.finish(_turn);
  _lastResponseBytes = _responseBytes;
  _turn = 0;
  _phase = Phase::Idle;
  _timerArmed = false;
  _peekSamples = 0;
  emit(TurnEventType::TurnComplete, TurnError::None);
  returnCredit(true);
}

void GatewayTurnSource::fail(TurnError error) {
  if (_phase == Phase::Idle) return;  // exactly once
  abandon();
  ++_c.turnsFailed;
  emit(TurnEventType::Error, error);
  returnCredit(true);
}

void GatewayTurnSource::abandon() {
  const uint32_t turn = _turn;
  _uplink.requestCancel(turn);
  _cancelled[_cancelledHead] = turn;
  _cancelledHead = (_cancelledHead + 1) % kCancelledMemory;
  _ids.finish(turn);
  _lastResponseBytes = _responseBytes;
  _turn = 0;
  _phase = Phase::Idle;
  _timerArmed = false;
  _satisfied = false;
  _peekSamples = 0;
  purgeRing();
  ++_generation;
  _events.clear();
}

void GatewayTurnSource::satisfy(uint32_t nowMs) {
  _satisfied = true;
  if (!_timerArmed) return;
  const uint32_t ms = nowMs - _turnEndSentMs;
  _c.lastFirstResponseMs = ms;
  if (ms > _c.maxFirstResponseMs) _c.maxFirstResponseMs = ms;
}

void GatewayTurnSource::returnCredit(bool force) {
  if (!_linkReady) return;
  const uint32_t pending = _credit.pendingReturn(force);
  if (pending == 0) return;
  if (_uplink.pushCredit(pending) == OutPush::Accepted) {
    _credit.commitReturn(pending);
    _c.creditReturned += pending;
  } else {
    ++_c.creditReturnBusy;
  }
}

bool GatewayTurnSource::wasCancelled(uint32_t turn) const {
  for (uint32_t i = 0; i < kCancelledMemory; ++i) {
    if (_cancelled[i] == turn && turn != 0) return true;
  }
  return false;
}

}  // namespace tth
