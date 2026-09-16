#pragma once

#include <stdint.h>

// AudioBus — the single owner of every microphone and speaker begin/end
// transition on the Core2.
//
// WHY THIS EXISTS
//
// The Core2's SPM1423 PDM microphone and NS4168 speaker amplifier are driven
// from the same I2S peripheral and share a clock pin. Phase 0's probe reported
// that simultaneous use *appeared* to work, but that result is not trusted:
// the project decision is to treat microphone and speaker operation as
// STRICTLY HALF-DUPLEX. Only one of them may be installed at any moment.
//
// Push-to-talk is naturally half-duplex, so this costs nothing: the child
// speaks, then the robot speaks. Barge-in still works because it is sequential
// (stop the speaker, then start the microphone).
//
// THE UNMATCHED-UNINSTALL BUG THIS PREVENTS
//
// Phase 0's throwaway diagnostic called M5.Mic.end() and M5.Speaker.end()
// unconditionally to "reset" the audio state. Ending a peripheral that was
// never installed makes the ESP-IDF driver log:
//
//     E (…) I2S: i2s_driver_uninstall(…): I2S port 1 has not installed
//
// AudioBus makes that impossible by construction: it tracks which peripheral
// it actually brought up, and only ever ends that one. No caller is permitted
// to call begin()/end() directly — that is the whole point of the boundary.
//
// This class is deliberately hardware-free so it can be unit tested on the
// host: the real M5Unified calls live behind IAudioDevice.

namespace tth {

enum class AudioOwner : uint8_t {
  None = 0,
  Mic,
  Speaker,
};

const char* toString(AudioOwner owner);

// The four hardware transitions AudioBus is allowed to perform. Implemented
// for real by M5AudioDevice (src/audio/), and by a recording fake in the
// native tests.
//
// Contract: micEnd()/speakerEnd() are only ever called for a peripheral whose
// matching begin() previously returned true.
class IAudioDevice {
 public:
  virtual ~IAudioDevice() {}

  virtual bool micBegin() = 0;
  virtual void micEnd() = 0;
  virtual bool speakerBegin() = 0;
  virtual void speakerEnd() = 0;
};

class AudioBus {
 public:
  explicit AudioBus(IAudioDevice& device);

  // Makes the microphone the exclusive owner. If the speaker currently owns
  // the bus it is ended FIRST, then the microphone is started — never the
  // other way around, and never both at once.
  //
  // Returns false if the microphone could not be started; in that case the
  // bus is left owned by nobody (the speaker has already been torn down and
  // must not be silently resurrected). Callers must treat false as an error
  // condition, not as "carry on" — in particular, barge-in must not show the
  // LISTENING face when this returns false.
  //
  // Idempotent: acquiring a bus the microphone already owns does nothing and
  // returns true.
  bool acquireMic();

  // Mirror image of acquireMic().
  bool acquireSpeaker();

  // Releases whatever is currently installed. A no-op when nobody owns the
  // bus — this is what prevents the unmatched i2s_driver_uninstall above.
  void releaseAll();

  AudioOwner owner() const { return _owner; }
  bool micOwns() const { return _owner == AudioOwner::Mic; }
  bool speakerOwns() const { return _owner == AudioOwner::Speaker; }

  // Diagnostics. A transition is one successful change of owner; a churning
  // count here during a single turn would indicate a state-machine bug.
  uint32_t transitions() const { return _transitions; }
  uint32_t failedAcquires() const { return _failedAcquires; }

 private:
  IAudioDevice& _device;
  AudioOwner _owner;
  uint32_t _transitions;
  uint32_t _failedAcquires;
};

}  // namespace tth
