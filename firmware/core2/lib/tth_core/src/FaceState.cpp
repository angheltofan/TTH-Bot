#include "tth/FaceState.h"

namespace tth {

const char* toString(FaceState state) {
  switch (state) {
    case FaceState::Ready:
      return "ready";
    case FaceState::Listening:
      return "listening";
    case FaceState::Waiting:
      return "waiting";
    case FaceState::Speaking:
      return "speaking";
    case FaceState::Error:
      return "error";
    case FaceState::Sleeping:
      return "sleeping";
  }
  return "invalid";
}

FaceState faceStateFor(ConversationState state) {
  switch (state) {
    // PHASE6_PLAN §9: booting, unprovisioned, connecting and link lost all
    // show Sleeping. (Before Step 6.1 Booting showed Ready; the robot now
    // cannot talk until it is online, so it must not look ready.)
    case ConversationState::Booting:
    case ConversationState::Disconnected:
    case ConversationState::Connecting:
      return FaceState::Sleeping;
    case ConversationState::Ready:
      return FaceState::Ready;
    case ConversationState::Listening:
      return FaceState::Listening;
    case ConversationState::Waiting:
      return FaceState::Waiting;
    case ConversationState::Speaking:
      return FaceState::Speaking;
    case ConversationState::Error:
      return FaceState::Error;
  }
  return FaceState::Error;
}

}  // namespace tth
