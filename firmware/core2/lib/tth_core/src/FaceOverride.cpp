#include "tth/FaceOverride.h"

namespace tth {

const FaceState kFaceCycle[kFaceCycleLength] = {
    FaceState::Ready,    FaceState::Listening, FaceState::Waiting,
    FaceState::Speaking, FaceState::Error,     FaceState::Sleeping};

const float kFixedSpeechLevels[kFixedLevelCount] = {0.0f, 0.25f, 0.5f, 0.75f,
                                                    1.0f};

int nextFixedLevelIndex(int current) {
  const int next = current + 1;
  // Past the loudest level, hand control back to the simulated envelope.
  return (next < 0 || next > kFixedLevelCount) ? 0 : next;
}

float fixedSpeechLevelFor(int index) {
  if (index < 0 || index >= kFixedLevelCount) return -1.0f;
  return kFixedSpeechLevels[index];
}

FaceOverride::FaceOverride(uint32_t cycleIntervalMs)
    : _cycleIntervalMs(cycleIntervalMs),
      _mode(Mode::None),
      _held(FaceState::Ready),
      _cycleStartedMs(0) {}

bool FaceOverride::handleKey(char key, uint32_t nowMs) {
  switch (key) {
    case 'r':
    case 'R':
      _mode = Mode::Hold;
      _held = FaceState::Ready;
      return true;
    case 'l':
    case 'L':
      _mode = Mode::Hold;
      _held = FaceState::Listening;
      return true;
    case 'w':
    case 'W':
      _mode = Mode::Hold;
      _held = FaceState::Waiting;
      return true;
    case 's':
    case 'S':
      _mode = Mode::Hold;
      _held = FaceState::Speaking;
      return true;
    case 'e':
    case 'E':
      _mode = Mode::Hold;
      _held = FaceState::Error;
      return true;
    case 'o':
    case 'O':
      _mode = Mode::Hold;
      _held = FaceState::Sleeping;
      return true;
    case 'a':
    case 'A':
      _mode = Mode::Cycle;
      _cycleStartedMs = nowMs;
      return true;
    default:
      return false;
  }
}

void FaceOverride::clear() { _mode = Mode::None; }

FaceState FaceOverride::faceFor(FaceState productionFace, uint32_t nowMs) {
  switch (_mode) {
    case Mode::None:
      return productionFace;
    case Mode::Hold:
      return _held;
    case Mode::Cycle: {
      const uint32_t elapsed = nowMs - _cycleStartedMs;
      const uint32_t index =
          (elapsed / _cycleIntervalMs) % static_cast<uint32_t>(kFaceCycleLength);
      return kFaceCycle[index];
    }
  }
  return productionFace;
}

}  // namespace tth
