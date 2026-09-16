#include "tth/AudioBus.h"

namespace tth {

const char* toString(AudioOwner owner) {
  switch (owner) {
    case AudioOwner::None:
      return "none";
    case AudioOwner::Mic:
      return "mic";
    case AudioOwner::Speaker:
      return "speaker";
  }
  return "invalid";
}

AudioBus::AudioBus(IAudioDevice& device)
    : _device(device),
      _owner(AudioOwner::None),
      _transitions(0),
      _failedAcquires(0) {}

bool AudioBus::acquireMic() {
  if (_owner == AudioOwner::Mic) return true;

  // Tear the speaker down before touching the microphone. Order matters: the
  // two cannot be installed at the same time, so overlapping them is exactly
  // the failure this class exists to prevent.
  if (_owner == AudioOwner::Speaker) {
    _device.speakerEnd();
    _owner = AudioOwner::None;
  }

  if (!_device.micBegin()) {
    // Leave the bus unowned rather than restoring the speaker. Restoring it
    // would hide a real fault behind audio that keeps playing, and the caller
    // needs to see the failure.
    _owner = AudioOwner::None;
    ++_failedAcquires;
    return false;
  }

  _owner = AudioOwner::Mic;
  ++_transitions;
  return true;
}

bool AudioBus::acquireSpeaker() {
  if (_owner == AudioOwner::Speaker) return true;

  if (_owner == AudioOwner::Mic) {
    _device.micEnd();
    _owner = AudioOwner::None;
  }

  if (!_device.speakerBegin()) {
    _owner = AudioOwner::None;
    ++_failedAcquires;
    return false;
  }

  _owner = AudioOwner::Speaker;
  ++_transitions;
  return true;
}

void AudioBus::releaseAll() {
  switch (_owner) {
    case AudioOwner::Mic:
      _device.micEnd();
      break;
    case AudioOwner::Speaker:
      _device.speakerEnd();
      break;
    case AudioOwner::None:
      // Nothing was installed, so nothing may be uninstalled.
      return;
  }
  _owner = AudioOwner::None;
}

}  // namespace tth
