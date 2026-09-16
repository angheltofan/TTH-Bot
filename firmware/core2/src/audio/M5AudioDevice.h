#pragma once

#include "tth/AudioBus.h"

namespace tth {

// The ONLY place in the firmware that is allowed to call M5.Mic.begin(),
// M5.Mic.end(), M5.Speaker.begin() or M5.Speaker.end().
//
// Everything else goes through AudioBus, which decides *when* those calls
// happen and guarantees they stay matched and mutually exclusive. Keeping the
// hardware calls behind this narrow class is what lets the arbitration logic
// itself be unit tested on the host.
//
// If you find yourself wanting to call M5.Mic/M5.Speaker begin/end anywhere
// else, that is the bug -- add the capability to AudioBus instead.
class M5AudioDevice : public IAudioDevice {
 public:
  // Applies the microphone sample rate the whole system is built around
  // (TTH_MIC_SAMPLE_RATE). Call once, before the first acquire.
  void configure();

  // M5.Speaker master volume, clamped to 0..255 (kSpeakerVolumeMax). Not a
  // begin/end transition: it only sets the level the speaker task uses.
  void setMasterVolume(uint32_t volume);
  uint8_t masterVolume() const;

  bool micBegin() override;
  void micEnd() override;
  bool speakerBegin() override;
  void speakerEnd() override;
};

}  // namespace tth
