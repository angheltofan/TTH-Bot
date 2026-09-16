#pragma once

#include <stdint.h>

#include "tth/AppState.h"

// The face states the robot renderer supports.
//
// Deliberate divergence from Flutter: mapConversationStateToExpression()
// (lib/features/robot/robot_expression.dart) maps `error` to the *neutral*
// expression on purpose, so that a Gemini session dropping between uses does
// not visibly dim the face. The Core2 specification explicitly asks for a
// distinct ERROR face, so Error maps to its own state here and is rendered in
// the dim cyan TthColors.faceCyanDim. This is a documented decision, not an
// oversight.
//
// Sleeping (Step 6.1, PHASE6_PLAN §9 / D4): booting, unprovisioned, connecting
// or link lost. Drowsy half-closed eyes in dim cyan, a slow blink, no bars —
// the one visible "not ready yet" cue, since the screen shows no text.

namespace tth {

enum class FaceState : uint8_t {
  Ready = 0,
  Listening,
  Waiting,
  Speaking,
  Error,
  Sleeping,
};

const char* toString(FaceState state);

// Pure mapping, so it is directly unit-testable without a renderer.
FaceState faceStateFor(ConversationState state);

}  // namespace tth
