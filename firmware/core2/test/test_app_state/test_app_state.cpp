// Host-side tests for the portable state definitions.

#include <unity.h>

#include "tth/AppState.h"
#include "tth/FaceState.h"

void setUp() {}
void tearDown() {}

static void conversation_state_names_are_stable() {
  TEST_ASSERT_EQUAL_STRING("booting",
                           tth::toString(tth::ConversationState::Booting));
  TEST_ASSERT_EQUAL_STRING("ready",
                           tth::toString(tth::ConversationState::Ready));
  TEST_ASSERT_EQUAL_STRING("listening",
                           tth::toString(tth::ConversationState::Listening));
  TEST_ASSERT_EQUAL_STRING("waiting",
                           tth::toString(tth::ConversationState::Waiting));
  TEST_ASSERT_EQUAL_STRING("speaking",
                           tth::toString(tth::ConversationState::Speaking));
  TEST_ASSERT_EQUAL_STRING("error",
                           tth::toString(tth::ConversationState::Error));
  TEST_ASSERT_EQUAL_STRING("disconnected",
                           tth::toString(tth::ConversationState::Disconnected));
  TEST_ASSERT_EQUAL_STRING("connecting",
                           tth::toString(tth::ConversationState::Connecting));
}

static void face_state_names_are_stable() {
  TEST_ASSERT_EQUAL_STRING("ready", tth::toString(tth::FaceState::Ready));
  TEST_ASSERT_EQUAL_STRING("listening",
                           tth::toString(tth::FaceState::Listening));
  TEST_ASSERT_EQUAL_STRING("waiting", tth::toString(tth::FaceState::Waiting));
  TEST_ASSERT_EQUAL_STRING("speaking",
                           tth::toString(tth::FaceState::Speaking));
  TEST_ASSERT_EQUAL_STRING("error", tth::toString(tth::FaceState::Error));
  TEST_ASSERT_EQUAL_STRING("sleeping", tth::toString(tth::FaceState::Sleeping));
}

static void connectivity_names_are_stable() {
  TEST_ASSERT_EQUAL_STRING("online", tth::toString(tth::Connectivity::Online));
  TEST_ASSERT_EQUAL_STRING("connecting",
                           tth::toString(tth::Connectivity::Connecting));
  TEST_ASSERT_EQUAL_STRING("offline", tth::toString(tth::Connectivity::Offline));
  TEST_ASSERT_EQUAL_STRING("failed", tth::toString(tth::Connectivity::Failed));
}

// PHASE6_PLAN §9 (D4): booting, unprovisioned, connecting and link lost all
// show the Sleeping face. Before Step 6.1 Booting showed Ready; the robot now
// cannot talk until it is online, so it must not look ready.
static void not_online_states_share_the_sleeping_face() {
  TEST_ASSERT_EQUAL(tth::FaceState::Sleeping,
                    tth::faceStateFor(tth::ConversationState::Booting));
  TEST_ASSERT_EQUAL(tth::FaceState::Sleeping,
                    tth::faceStateFor(tth::ConversationState::Disconnected));
  TEST_ASSERT_EQUAL(tth::FaceState::Sleeping,
                    tth::faceStateFor(tth::ConversationState::Connecting));
}

static void ready_is_only_the_online_idle_face() {
  TEST_ASSERT_EQUAL(tth::FaceState::Ready,
                    tth::faceStateFor(tth::ConversationState::Ready));
}

static void active_states_map_one_to_one() {
  TEST_ASSERT_EQUAL(tth::FaceState::Listening,
                    tth::faceStateFor(tth::ConversationState::Listening));
  TEST_ASSERT_EQUAL(tth::FaceState::Waiting,
                    tth::faceStateFor(tth::ConversationState::Waiting));
  TEST_ASSERT_EQUAL(tth::FaceState::Speaking,
                    tth::faceStateFor(tth::ConversationState::Speaking));
}

// Documented divergence from Flutter, which maps `error` to the neutral face
// on purpose. The Core2 spec requires a distinct ERROR face, so this must NOT
// collapse to Ready.
static void error_has_its_own_face_unlike_flutter() {
  const tth::FaceState face = tth::faceStateFor(tth::ConversationState::Error);
  TEST_ASSERT_EQUAL(tth::FaceState::Error, face);
  TEST_ASSERT_TRUE(face != tth::FaceState::Ready);
  TEST_ASSERT_TRUE(face != tth::FaceState::Sleeping);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(conversation_state_names_are_stable);
  RUN_TEST(face_state_names_are_stable);
  RUN_TEST(connectivity_names_are_stable);
  RUN_TEST(not_online_states_share_the_sleeping_face);
  RUN_TEST(ready_is_only_the_online_idle_face);
  RUN_TEST(active_states_map_one_to_one);
  RUN_TEST(error_has_its_own_face_unlike_flutter);
  return UNITY_END();
}
