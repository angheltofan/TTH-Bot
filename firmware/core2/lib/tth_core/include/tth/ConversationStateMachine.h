#pragma once

#include <stdint.h>

#include "tth/AppState.h"

// The conversation state machine.
//
// Kept portable (no M5Unified, no clock of its own) so every transition is
// unit-testable on the host. App owns one of these and does nothing but feed
// it events and log the result.
//
// TRANSITIONS
//
//     Ready     --press-------------->  Listening
//     Listening --release------------>  Waiting
//     Waiting   --speech start------->  Speaking
//     Speaking  --turn complete------>  <rest>
//     Waiting   --turn complete------>  <rest>    (a response with no speech)
//     Speaking  --press-------------->  Listening (barge-in)
//     any       --error-------------->  Error
//     Error     --press-------------->  Listening (retry)
//
// CONNECTIVITY (Step 6.1, 6.2). <rest> is the idle state for the link:
//     Online -> Ready,  Connecting -> Connecting,  Offline -> Disconnected,
//     Failed -> Error (AuthRejected / ProtocolMismatch: persistent, D4).
// A link change moves the machine only while it is idle (Ready, Connecting,
// Disconnected, or an Error that the link itself caused); an active turn and a
// local Error are never interrupted by it. A press is refused whenever the
// link is not Online -- the robot cannot talk until it is (PHASE6_PLAN §9).
// Connectivity defaults to Online, so a machine that is never told about a
// link behaves exactly as before Step 6.1.
//
// The machine records WHAT the robot is doing, never whether the hardware
// allows it. App calls onPress() only after the microphone is actually
// recording, so the face can never claim LISTENING early.

namespace tth {

class ConversationStateMachine {
 public:
  ConversationStateMachine();

  ConversationState state() const { return _state; }
  Connectivity connectivity() const { return _connectivity; }

  // True while the current Error was caused by the link (Failed), not a turn.
  bool errorFromLink() const { return _state == ConversationState::Error && _linkError; }

  // Completes start-up: Booting -> the rest state for the current link.
  bool markReady();

  // Fails the current turn: any state -> Error. Persistent until a press.
  bool onError();

  // A TRANSIENT failure (PHASE6_PLAN §9, D4: timeout, drop mid-turn, gateway
  // error, credit violation): Error for at least `holdMs`, then tick() returns
  // the machine to the rest state for the link (Ready if online, else
  // Connecting/Disconnected). A press while online still retries at once.
  bool onTransientError(uint32_t nowMs, uint32_t holdMs);

  // Once per loop. True when a transient Error ended.
  bool tick(uint32_t nowMs);

  bool transientError() const {
    return _state == ConversationState::Error && _transient;
  }

  // Records the link. Moves the machine only while idle.
  bool setConnectivity(Connectivity connectivity);

  // Each returns true when the state actually changed, so the caller can log
  // exactly the real transitions and stay quiet otherwise.
  bool onPress(uint32_t nowMs);
  bool onRelease(uint32_t nowMs);
  bool onSpeechStart();
  // The response is over (or there was none). Also used when a barge-in is
  // abandoned because the button was released before the microphone started.
  bool onTurnComplete();

  static bool isIdle(ConversationState state);

 private:
  bool enter(ConversationState next);
  bool rest();
  ConversationState restState() const;

  ConversationState _state;
  Connectivity _connectivity;
  bool _linkError;
  bool _transient;
  uint32_t _transientUntilMs;
};

}  // namespace tth
