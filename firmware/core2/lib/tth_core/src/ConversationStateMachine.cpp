#include "tth/ConversationStateMachine.h"

namespace tth {

ConversationStateMachine::ConversationStateMachine()
    : _state(ConversationState::Booting),
      _connectivity(Connectivity::Online),
      _linkError(false),
      _transient(false),
      _transientUntilMs(0) {}

bool ConversationStateMachine::enter(ConversationState next) {
  if (_state == next) return false;
  _state = next;
  return true;
}

bool ConversationStateMachine::isIdle(ConversationState state) {
  return state == ConversationState::Ready ||
         state == ConversationState::Connecting ||
         state == ConversationState::Disconnected;
}

ConversationState ConversationStateMachine::restState() const {
  switch (_connectivity) {
    case Connectivity::Online:
      return ConversationState::Ready;
    case Connectivity::Connecting:
      return ConversationState::Connecting;
    case Connectivity::Offline:
      return ConversationState::Disconnected;
    case Connectivity::Failed:
      return ConversationState::Error;
  }
  return ConversationState::Disconnected;
}

bool ConversationStateMachine::rest() {
  const ConversationState next = restState();
  _linkError = (next == ConversationState::Error);
  return enter(next);
}

bool ConversationStateMachine::markReady() {
  if (_state != ConversationState::Booting) return false;
  return rest();
}

bool ConversationStateMachine::onError() {
  _linkError = false;
  _transient = false;
  return enter(ConversationState::Error);
}

bool ConversationStateMachine::onTransientError(uint32_t nowMs, uint32_t holdMs) {
  _linkError = false;
  _transient = true;
  _transientUntilMs = nowMs + holdMs;
  return enter(ConversationState::Error);
}

bool ConversationStateMachine::tick(uint32_t nowMs) {
  if (_state != ConversationState::Error || !_transient) return false;
  if (static_cast<int32_t>(nowMs - _transientUntilMs) < 0) return false;
  _transient = false;
  return rest();
}

bool ConversationStateMachine::setConnectivity(Connectivity connectivity) {
  _connectivity = connectivity;
  if (!isIdle(_state) && !errorFromLink()) return false;
  return rest();
}

bool ConversationStateMachine::onPress(uint32_t nowMs) {
  (void)nowMs;
  // Not online: the robot cannot talk, so it must not start listening.
  if (_connectivity != Connectivity::Online) return false;

  // Booting is accepted as well as Ready: treating a press during start-up as
  // "go" rather than dropping it on the floor is the friendlier behaviour.
  //
  // Error is accepted too, and is the only way out of a local error: a press
  // retries the turn.
  //
  // Speaking is barge-in. App calls this only once the speaker has been
  // stopped and released and the microphone is recording.
  if (_state != ConversationState::Ready &&
      _state != ConversationState::Booting &&
      _state != ConversationState::Error &&
      _state != ConversationState::Speaking) {
    // A press during Listening or Waiting is ignored.
    return false;
  }
  _linkError = false;
  _transient = false;
  return enter(ConversationState::Listening);
}

bool ConversationStateMachine::onRelease(uint32_t nowMs) {
  (void)nowMs;
  if (_state != ConversationState::Listening) return false;
  return enter(ConversationState::Waiting);
}

bool ConversationStateMachine::onSpeechStart() {
  if (_state != ConversationState::Waiting) return false;
  return enter(ConversationState::Speaking);
}

bool ConversationStateMachine::onTurnComplete() {
  if (_state != ConversationState::Speaking &&
      _state != ConversationState::Waiting) {
    return false;
  }
  return rest();
}

}  // namespace tth
