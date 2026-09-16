#include "tth/PcmPlayer.h"

#include "tth/PcmProcessing.h"

namespace tth {

const char* toString(PlayerState state) {
  switch (state) {
    case PlayerState::Idle:
      return "idle";
    case PlayerState::Streaming:
      return "streaming";
    case PlayerState::Ending:
      return "ending";
    case PlayerState::Stopping:
      return "stopping";
    case PlayerState::Drained:
      return "drained";
  }
  return "invalid";
}

const char* toString(PlayerPush result) {
  switch (result) {
    case PlayerPush::Accepted:
      return "accepted";
    case PlayerPush::Busy:
      return "busy";
    case PlayerPush::FormatMismatch:
      return "format mismatch";
    case PlayerPush::NotOpen:
      return "not open";
  }
  return "invalid";
}

const char* toString(PlaybackEnd reason) {
  switch (reason) {
    case PlaybackEnd::None:
      return "none";
    case PlaybackEnd::Completed:
      return "completed";
    case PlaybackEnd::Cancelled:
      return "cancelled";
    case PlaybackEnd::Error:
      return "error";
  }
  return "invalid";
}

PcmPlayer::PcmPlayer(AudioBus& bus, ISpeakerOutput& output,
                     uint32_t drainTimeoutMs)
    : _bus(bus),
      _output(output),
      _drainTimeoutMs(drainTimeoutMs),
      _slotSamples(0),
      _fillIdx(0),
      _playIdx(0),
      _doneIdx(0),
      _filled(0),
      _playing(0),
      _state(PlayerState::Idle),
      _endReason(PlaybackEnd::None),
      _format(monoS16(0)),
      _firstAccepted(false),
      _firstAudioPending(false),
      _inUnderrun(false),
      _drainStartedMs(0),
      _drainClockRunning(false),
      _lastDrainMs(0) {
  for (uint32_t i = 0; i < kSlots; ++i) {
    _slots[i] = nullptr;
    _slotCount[i] = 0;
    _slotLevel[i] = 0.0f;
    _slotState[i] = SlotState::Free;
  }
  _metrics = PlaybackMetrics();
}

void PcmPlayer::begin(int16_t* slotA, int16_t* slotB, int16_t* slotC,
                      uint32_t slotSamples) {
  _slots[0] = slotA;
  _slots[1] = slotB;
  _slots[2] = slotC;
  const bool all = slotA != nullptr && slotB != nullptr && slotC != nullptr;
  _slotSamples = all ? slotSamples : 0;
}

bool PcmPlayer::openStream(const AudioFormat& format, uint32_t nowMs) {
  (void)nowMs;
  if (_state != PlayerState::Idle) return false;

  // Per-stream figures start here, so a summary describes this response only.
  _metrics = PlaybackMetrics();

  if (!isPlayable(format)) {
    ++_metrics.formatRejects;
    return false;
  }
  if (_slotSamples == 0) return false;

  // Half duplex, and the order matters: a capture that is still draining owns
  // the microphone, and its task may still be writing its buffers. Taking the
  // speaker now would tear that down underneath it.
  if (_bus.owner() == AudioOwner::Mic) return false;

  if (!_bus.acquireSpeaker()) return false;

  for (uint32_t i = 0; i < kSlots; ++i) {
    _slotState[i] = SlotState::Free;
    _slotCount[i] = 0;
    _slotLevel[i] = 0.0f;
  }
  _fillIdx = 0;
  _playIdx = 0;
  _doneIdx = 0;
  _filled = 0;
  _playing = 0;
  _format = format;
  _endReason = PlaybackEnd::None;
  _firstAccepted = false;
  _firstAudioPending = false;
  _inUnderrun = false;
  _drainClockRunning = false;
  _lastDrainMs = 0;
  _state = PlayerState::Streaming;
  return true;
}

bool PcmPlayer::canAccept() const {
  return _state == PlayerState::Streaming &&
         _slotState[_fillIdx] == SlotState::Free;
}

PlayerPush PcmPlayer::submit(const AudioChunk& chunk, uint32_t& accepted) {
  accepted = 0;
  if (_state != PlayerState::Streaming) return PlayerPush::NotOpen;

  // Exact match or nothing. This is the check that makes it impossible for a
  // 16 kHz buffer to reach a stream the speaker is playing at 24 kHz.
  if (!sameFormat(chunk.format, _format)) {
    ++_metrics.formatRejects;
    return PlayerPush::FormatMismatch;
  }
  if (chunk.samples == nullptr || chunk.count == 0) {
    return PlayerPush::Accepted;
  }

  // Ring full: take nothing. The caller has not consumed these samples, so
  // they are simply offered again -- a full ring loses no audio.
  if (_slotState[_fillIdx] != SlotState::Free) return PlayerPush::Busy;

  const uint32_t count =
      (chunk.count < _slotSamples) ? chunk.count : _slotSamples;
  int16_t* slot = _slots[_fillIdx];
  for (uint32_t i = 0; i < count; ++i) {
    const int16_t sample = chunk.samples[i];
    slot[i] = sample;
    const int32_t magnitude =
        (sample < 0) ? -static_cast<int32_t>(sample) : sample;
    if (magnitude > _metrics.inputPeak) _metrics.inputPeak = magnitude;
  }

  // Measured now, but only REPORTED once the speaker is actually playing this
  // slot (see currentLevel), so the bars follow the sound rather than the
  // queue.
  _slotLevel[_fillIdx] = measureLevels(slot, count).rms;
  _slotCount[_fillIdx] = count;
  _slotState[_fillIdx] = SlotState::Filled;
  _fillIdx = next(_fillIdx);
  ++_filled;

  ++_metrics.queued;
  _metrics.samplesQueued += count;
  accepted = count;
  return PlayerPush::Accepted;
}

void PcmPlayer::markEndOfStream(uint32_t nowMs) {
  (void)nowMs;
  if (_state != PlayerState::Streaming) return;
  _state = PlayerState::Ending;
  _drainClockRunning = false;
}

void PcmPlayer::cancel(uint32_t nowMs, PlaybackEnd reason) {
  if (_state == PlayerState::Idle || _state == PlayerState::Drained ||
      _state == PlayerState::Stopping) {
    return;
  }

  // Everything not yet handed to the speaker is ours alone: drop it. Slots the
  // speaker holds stay Playing -- its task may still be reading them -- and
  // are freed only once it lets go.
  for (uint32_t i = 0; i < kSlots; ++i) {
    if (_slotState[i] == SlotState::Filled) _slotState[i] = SlotState::Free;
  }
  _filled = 0;
  _fillIdx = _playIdx;

  _endReason = reason;
  _state = PlayerState::Stopping;
  _drainStartedMs = nowMs;
  _drainClockRunning = true;
  _output.stop();
}

void PcmPlayer::retireFinished(uint32_t occupied) {
  // The speaker holds fewer requests than we handed it: the oldest ones have
  // finished playing and their slots are ours again.
  if (occupied >= _playing) return;
  uint32_t finished = _playing - occupied;
  while (finished > 0) {
    _slotState[_doneIdx] = SlotState::Free;
    _doneIdx = next(_doneIdx);
    --_playing;
    ++_metrics.played;
    --finished;
  }
}

void PcmPlayer::queueFilled() {
  while (_filled > 0) {
    // THE GUARD. Re-read immediately before every play(): with both speaker
    // slots occupied, M5Unified's playRaw() would spin until one frees -- a
    // whole chunk of blocked loop.
    if (_output.slotsOccupied() >= kSpeakerQueueDepth) break;

    const uint32_t index = _playIdx;
    // The rate is always the stream's, passed explicitly on every call.
    if (!_output.play(_slots[index], _slotCount[index], _format.sampleRate)) {
      ++_metrics.playRefusals;
      break;
    }

    _slotState[index] = SlotState::Playing;
    _playIdx = next(index);
    --_filled;
    ++_playing;
    if (_slotLevel[index] > _metrics.maxLevel) {
      _metrics.maxLevel = _slotLevel[index];
    }

    if (!_firstAccepted) {
      _firstAccepted = true;
      _firstAudioPending = true;
    }
  }
}

void PcmPlayer::enterDrained(uint32_t nowMs) {
  for (uint32_t i = 0; i < kSlots; ++i) _slotState[i] = SlotState::Free;
  _fillIdx = 0;
  _playIdx = 0;
  _doneIdx = 0;
  _filled = 0;
  _playing = 0;
  _lastDrainMs = _drainClockRunning ? (nowMs - _drainStartedMs) : 0;
  _drainClockRunning = false;
  _state = PlayerState::Drained;
}

void PcmPlayer::service(uint32_t nowMs) {
  if (_state == PlayerState::Idle || _state == PlayerState::Drained) return;

  retireFinished(_output.slotsOccupied());

  if (_state == PlayerState::Streaming || _state == PlayerState::Ending) {
    queueFilled();
  }

  if (_state == PlayerState::Streaming && _firstAccepted) {
    // Ran dry mid-stream: the speaker has finished everything and nothing is
    // waiting. Counted once per episode, not once per loop.
    const bool dry = (_filled == 0 && _playing == 0);
    if (dry && !_inUnderrun) ++_metrics.underruns;
    _inUnderrun = dry;
  }

  if (_state == PlayerState::Ending && _filled == 0) {
    if (!_drainClockRunning) {
      _drainStartedMs = nowMs;
      _drainClockRunning = true;
    }
    if (_playing == 0 && _output.slotsOccupied() == 0) {
      _endReason = PlaybackEnd::Completed;
      enterDrained(nowMs);
      return;
    }
    if ((nowMs - _drainStartedMs) >= _drainTimeoutMs) {
      // Everything was queued but the speaker never reported done. Stop it
      // explicitly and let the Stopping path finish the job.
      ++_metrics.drainTimeouts;
      _endReason = PlaybackEnd::Completed;
      _state = PlayerState::Stopping;
      _drainStartedMs = nowMs;
      _output.stop();
      return;
    }
  }

  if (_state == PlayerState::Stopping) {
    if (_output.slotsOccupied() == 0) {
      enterDrained(nowMs);
    } else if ((nowMs - _drainStartedMs) >= _drainTimeoutMs) {
      // Give up waiting. Safe: release() -> M5.Speaker.end() stops the task
      // before any slot can be reused.
      ++_metrics.drainTimeouts;
      enterDrained(nowMs);
    }
  }
}

bool PcmPlayer::release() {
  if (_state != PlayerState::Drained) return false;
  _bus.releaseAll();
  _state = PlayerState::Idle;
  return true;
}

bool PcmPlayer::consumeFirstAudio() {
  if (!_firstAudioPending) return false;
  _firstAudioPending = false;
  return true;
}

float PcmPlayer::currentLevel() const {
  if (_state != PlayerState::Streaming && _state != PlayerState::Ending) {
    return 0.0f;
  }
  if (_playing == 0) return 0.0f;
  // The oldest slot the speaker holds is the one being heard.
  return _slotLevel[_doneIdx];
}

}  // namespace tth
