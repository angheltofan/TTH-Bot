#pragma once

#include <stdint.h>

// Portable conversation state. Mirrors ConversationState in the Flutter app
// (lib/features/voice/voice_session_controller.dart) so the two embodiments
// stay reviewable side by side.
//
// Step 6.1 adds the Flutter app's `disconnected` and `connecting`: the robot
// now depends on a network link. FaceState stays a separate enum with an
// explicit mapping (both map to the Sleeping face).

namespace tth {

enum class ConversationState : uint8_t {
  // Before setup() has finished. Not reachable once the main loop is running.
  Booting = 0,
  // Idle and online, waiting for the push-to-talk input to be held.
  Ready,
  // Push-to-talk held; the microphone owns the audio bus.
  Listening,
  // Turn handed off; waiting for the first speech audio to come back.
  Waiting,
  // Playing AI speech; the speaker owns the audio bus.
  Speaking,
  // Something failed. Cleared by the next push-to-talk press (when online).
  Error,
  // Idle with no network link: unprovisioned, or waiting to retry.
  Disconnected,
  // Idle while the link is being established.
  Connecting,
};

// Stable lowercase identifiers, safe to log. Never returns null.
const char* toString(ConversationState state);

// The network link as the conversation sees it.
enum class Connectivity : uint8_t {
  Online = 0,
  Connecting,
  Offline,
  // The gateway refused this robot (AuthRejected) or speaks another protocol
  // (ProtocolMismatch): a persistent ERROR, not a transient Sleeping
  // (PHASE6_PLAN D4).
  Failed,
};

const char* toString(Connectivity connectivity);

}  // namespace tth
