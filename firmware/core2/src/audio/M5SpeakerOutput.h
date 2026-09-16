#pragma once

#include <stdint.h>

#include "tth/PcmPlayer.h"

namespace tth {

// The speaker, as PcmPlayer sees it.
//
// THE ONLY PLACE IN THE FIRMWARE THAT CALLS M5.Speaker.playRaw().
//
// Two guards run immediately before that call, every time:
//
//  1. M5.Speaker.isPlaying(ch) < 2. playRaw() spins inside M5Unified until a
//     slot frees (Speaker_Class.cpp _set_next_wav); with both occupied that is
//     a whole chunk of blocked loop. PcmPlayer already checks this; checking
//     again here, at the call itself, is what makes it an invariant of the
//     firmware rather than of one caller.
//
//  2. M5.Speaker.isRunning(). playRaw() calls Speaker.begin() by itself if the
//     speaker is not running -- which would start the speaker behind
//     AudioBus's back, possibly while the microphone owns the shared I2S
//     hardware. Refused instead.
//
// begin()/end() are NOT here: they belong to M5AudioDevice, via AudioBus.
class M5SpeakerOutput : public ISpeakerOutput {
 public:
  explicit M5SpeakerOutput(uint8_t channel);

  uint32_t slotsOccupied() const override;
  bool play(const int16_t* samples, uint32_t count,
            uint32_t sampleRate) override;
  void stop() override;

  // The next play() is the first of a new stream, and is timed under its own
  // label: the first playRaw() after Speaker.begin() is where a priming cost,
  // if there is one, would show.
  void markNewStream() { _firstOfStream = true; }

  // Times either guard refused. Both should stay at zero; a non-zero value
  // means a caller broke the contract and the guard caught it.
  uint32_t busyRefusals() const { return _busyRefusals; }
  uint32_t notRunningRefusals() const { return _notRunningRefusals; }

 private:
  const uint8_t _channel;
  bool _firstOfStream;
  uint32_t _busyRefusals;
  uint32_t _notRunningRefusals;
};

}  // namespace tth
