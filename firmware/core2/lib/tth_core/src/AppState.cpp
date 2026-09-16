#include "tth/AppState.h"

namespace tth {

const char* toString(ConversationState state) {
  switch (state) {
    case ConversationState::Booting:
      return "booting";
    case ConversationState::Ready:
      return "ready";
    case ConversationState::Listening:
      return "listening";
    case ConversationState::Waiting:
      return "waiting";
    case ConversationState::Speaking:
      return "speaking";
    case ConversationState::Error:
      return "error";
    case ConversationState::Disconnected:
      return "disconnected";
    case ConversationState::Connecting:
      return "connecting";
  }
  return "invalid";
}

const char* toString(Connectivity connectivity) {
  switch (connectivity) {
    case Connectivity::Online:
      return "online";
    case Connectivity::Connecting:
      return "connecting";
    case Connectivity::Offline:
      return "offline";
    case Connectivity::Failed:
      return "failed";
  }
  return "invalid";
}

}  // namespace tth
