#include "audio/M5SpeakerOutput.h"

#include <M5Unified.h>

#include "diag/BlockTimer.h"

namespace tth {

M5SpeakerOutput::M5SpeakerOutput(uint8_t channel)
    : _channel(channel),
      _firstOfStream(true),
      _busyRefusals(0),
      _notRunningRefusals(0) {}

uint32_t M5SpeakerOutput::slotsOccupied() const {
  // A COUNT of occupied request slots (0..2), not a bool -- the same shape,
  // and the same trap, as Mic.isRecording(). Reads atomics; never blocks.
  return static_cast<uint32_t>(M5.Speaker.isPlaying(_channel));
}

bool M5SpeakerOutput::play(const int16_t* samples, uint32_t count,
                           uint32_t sampleRate) {
  // Guard 2: never let playRaw() start the speaker by itself.
  if (!M5.Speaker.isRunning()) {
    ++_notRunningRefusals;
    return false;
  }

  // Guard 1, THE speaker invariant: fewer than two requests held, checked at
  // the call itself. With two held, playRaw() would spin until one frees.
  if (M5.Speaker.isPlaying(_channel) >= kSpeakerQueueDepth) {
    ++_busyRefusals;
    return false;
  }

  bool accepted = false;
  {
    ::tth::diag::BlockTimer timer(_firstOfStream ? "spk.playRaw#1"
                                                 : "spk.playRaw");
    // Mono, played once, on our own channel, never pre-empting what is
    // already queued. The rate is the stream's, passed explicitly.
    accepted = M5.Speaker.playRaw(samples, count, sampleRate, false, 1,
                                  _channel, false);
  }
  if (accepted) _firstOfStream = false;
  return accepted;
}

void M5SpeakerOutput::stop() {
  // Asks the speaker task to stop this channel. It goes through the same
  // slot-claiming code as playRaw(), where it can pre-empt a queued request
  // rather than wait -- expected to be immediate, but timed, not assumed.
  TTH_TIME_BLOCK("M5.Speaker.stop");
  M5.Speaker.stop(_channel);
}

}  // namespace tth
