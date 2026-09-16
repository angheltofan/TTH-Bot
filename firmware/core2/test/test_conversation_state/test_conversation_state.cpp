// Host-side tests for the conversation state transitions.
//
//     Ready     --press-------------->  Listening
//     Listening --release------------>  Waiting
//     Waiting   --speech start------->  Speaking
//     Speaking  --turn complete------>  Ready
//     Waiting   --turn complete------>  Ready     (no speech)
//     Speaking  --press-------------->  Listening (barge-in)
//     any       --error-------------->  Error
//     Error     --press-------------->  Listening (retry)
//
// Phase 5 removed the Phase 2 Waiting timeout. The tests that pinned that
// timer now pin the event that replaced it: Waiting ends when the turn source
// delivers speech (or completes with none), never on a clock.

#include <unity.h>

#include "tth/AppState.h"
#include "tth/ConversationStateMachine.h"

namespace {

tth::ConversationStateMachine makeReadyMachine() {
  tth::ConversationStateMachine machine;
  machine.markReady();
  return machine;
}

// Ready -> Listening -> Waiting.
tth::ConversationStateMachine makeWaitingMachine() {
  tth::ConversationStateMachine machine = makeReadyMachine();
  machine.onPress(1000);
  machine.onRelease(1500);
  return machine;
}

}  // namespace

void setUp() {}
void tearDown() {}

static void starts_in_booting() {
  tth::ConversationStateMachine machine;
  TEST_ASSERT_EQUAL(tth::ConversationState::Booting, machine.state());
}

static void mark_ready_leaves_booting_exactly_once() {
  tth::ConversationStateMachine machine;

  TEST_ASSERT_TRUE(machine.markReady());
  TEST_ASSERT_EQUAL(tth::ConversationState::Ready, machine.state());

  // Reports no change the second time, so nothing is logged twice.
  TEST_ASSERT_FALSE(machine.markReady());
}

static void press_moves_ready_to_listening() {
  tth::ConversationStateMachine machine = makeReadyMachine();

  TEST_ASSERT_TRUE(machine.onPress(1000));
  TEST_ASSERT_EQUAL(tth::ConversationState::Listening, machine.state());
}

static void release_moves_listening_to_waiting() {
  tth::ConversationStateMachine machine = makeReadyMachine();
  machine.onPress(1000);

  TEST_ASSERT_TRUE(machine.onRelease(1500));
  TEST_ASSERT_EQUAL(tth::ConversationState::Waiting, machine.state());
}

// Replaces "waiting returns to ready after the hold": there is no hold any
// more. Waiting stays Waiting until the source says something happened.
static void waiting_does_not_end_on_its_own() {
  tth::ConversationStateMachine machine = makeWaitingMachine();

  // No clock-driven transition exists to call; the only ways out are events.
  TEST_ASSERT_EQUAL(tth::ConversationState::Waiting, machine.state());
  TEST_ASSERT_FALSE(machine.onRelease(999999));
  TEST_ASSERT_EQUAL(tth::ConversationState::Waiting, machine.state());
}

static void speech_start_moves_waiting_to_speaking() {
  tth::ConversationStateMachine machine = makeWaitingMachine();

  TEST_ASSERT_TRUE(machine.onSpeechStart());
  TEST_ASSERT_EQUAL(tth::ConversationState::Speaking, machine.state());
}

static void turn_complete_moves_speaking_to_ready() {
  tth::ConversationStateMachine machine = makeWaitingMachine();
  machine.onSpeechStart();

  TEST_ASSERT_TRUE(machine.onTurnComplete());
  TEST_ASSERT_EQUAL(tth::ConversationState::Ready, machine.state());
}

// A response with no audio at all still ends the turn.
static void turn_complete_without_speech_moves_waiting_to_ready() {
  tth::ConversationStateMachine machine = makeWaitingMachine();

  TEST_ASSERT_TRUE(machine.onTurnComplete());
  TEST_ASSERT_EQUAL(tth::ConversationState::Ready, machine.state());
}

// Speech that arrives when nobody is waiting for it must not fabricate a
// Speaking state.
static void speech_start_outside_waiting_is_ignored() {
  tth::ConversationStateMachine machine = makeReadyMachine();
  TEST_ASSERT_FALSE(machine.onSpeechStart());
  TEST_ASSERT_EQUAL(tth::ConversationState::Ready, machine.state());

  machine.onPress(1000);
  TEST_ASSERT_FALSE(machine.onSpeechStart());
  TEST_ASSERT_EQUAL(tth::ConversationState::Listening, machine.state());
}

static void turn_complete_outside_a_response_is_ignored() {
  tth::ConversationStateMachine machine = makeReadyMachine();
  TEST_ASSERT_FALSE(machine.onTurnComplete());

  machine.onPress(1000);
  TEST_ASSERT_FALSE(machine.onTurnComplete());
  TEST_ASSERT_EQUAL(tth::ConversationState::Listening, machine.state());
}

// Barge-in. App calls onPress() only after the speaker has been released and
// the microphone is recording; the machine then goes straight to Listening.
static void press_while_speaking_is_barge_in() {
  tth::ConversationStateMachine machine = makeWaitingMachine();
  machine.onSpeechStart();

  TEST_ASSERT_TRUE(machine.onPress(3000));
  TEST_ASSERT_EQUAL(tth::ConversationState::Listening, machine.state());
}

static void press_while_listening_is_ignored() {
  tth::ConversationStateMachine machine = makeReadyMachine();
  machine.onPress(1000);

  TEST_ASSERT_FALSE(machine.onPress(1100));
  TEST_ASSERT_EQUAL(tth::ConversationState::Listening, machine.state());
}

static void press_while_waiting_is_ignored() {
  tth::ConversationStateMachine machine = makeWaitingMachine();

  TEST_ASSERT_FALSE(machine.onPress(1600));
  TEST_ASSERT_EQUAL(tth::ConversationState::Waiting, machine.state());
}

// A release with no matching press must not move the machine anywhere --
// this is what stops a stray edge from fabricating a turn.
static void release_without_a_press_is_ignored() {
  tth::ConversationStateMachine machine = makeReadyMachine();

  TEST_ASSERT_FALSE(machine.onRelease(1000));
  TEST_ASSERT_EQUAL(tth::ConversationState::Ready, machine.state());
}

static void release_while_waiting_is_ignored() {
  tth::ConversationStateMachine machine = makeWaitingMachine();

  TEST_ASSERT_FALSE(machine.onRelease(1600));
  TEST_ASSERT_EQUAL(tth::ConversationState::Waiting, machine.state());
}

// A forced release from the maximum-hold timeout is delivered as an ordinary
// release, so the machine must complete the turn exactly the same way.
static void a_forced_release_completes_the_turn_normally() {
  tth::ConversationStateMachine machine = makeReadyMachine();
  machine.onPress(1000);

  TEST_ASSERT_TRUE(machine.onRelease(1000 + 45000));
  TEST_ASSERT_EQUAL(tth::ConversationState::Waiting, machine.state());
  TEST_ASSERT_TRUE(machine.onSpeechStart());
  TEST_ASSERT_TRUE(machine.onTurnComplete());
  TEST_ASSERT_EQUAL(tth::ConversationState::Ready, machine.state());
}

static void error_is_reachable_from_every_state_and_a_press_retries() {
  tth::ConversationStateMachine machine = makeReadyMachine();
  TEST_ASSERT_TRUE(machine.onError());
  TEST_ASSERT_EQUAL(tth::ConversationState::Error, machine.state());
  TEST_ASSERT_TRUE(machine.onPress(1000));
  TEST_ASSERT_EQUAL(tth::ConversationState::Listening, machine.state());

  TEST_ASSERT_TRUE(machine.onError());
  TEST_ASSERT_TRUE(machine.onPress(2000));
  machine.onRelease(2500);
  TEST_ASSERT_TRUE(machine.onError());
  TEST_ASSERT_EQUAL(tth::ConversationState::Error, machine.state());

  machine.onPress(3000);
  machine.onRelease(3500);
  machine.onSpeechStart();
  TEST_ASSERT_TRUE(machine.onError());
  TEST_ASSERT_EQUAL(tth::ConversationState::Error, machine.state());
}

static void several_turns_run_back_to_back() {
  tth::ConversationStateMachine machine = makeReadyMachine();
  uint32_t t = 1000;

  for (int turn = 0; turn < 5; ++turn) {
    TEST_ASSERT_TRUE(machine.onPress(t));
    TEST_ASSERT_EQUAL(tth::ConversationState::Listening, machine.state());

    t += 400;
    TEST_ASSERT_TRUE(machine.onRelease(t));
    TEST_ASSERT_EQUAL(tth::ConversationState::Waiting, machine.state());

    TEST_ASSERT_TRUE(machine.onSpeechStart());
    TEST_ASSERT_EQUAL(tth::ConversationState::Speaking, machine.state());

    TEST_ASSERT_TRUE(machine.onTurnComplete());
    TEST_ASSERT_EQUAL(tth::ConversationState::Ready, machine.state());

    t += 100;
  }
}

// Replaces "waiting survives a millis rollover": with no timer left in the
// machine, a turn spanning the wrap behaves exactly like any other.
static void a_turn_across_the_millis_rollover_behaves_normally() {
  tth::ConversationStateMachine machine = makeReadyMachine();

  const uint32_t nearMax = 0xFFFFFF00u;
  TEST_ASSERT_TRUE(machine.onPress(nearMax));
  TEST_ASSERT_TRUE(machine.onRelease(nearMax + 0x200u));  // wrapped
  TEST_ASSERT_TRUE(machine.onSpeechStart());
  TEST_ASSERT_TRUE(machine.onTurnComplete());
  TEST_ASSERT_EQUAL(tth::ConversationState::Ready, machine.state());
}

// --- Step 6.1: connectivity --------------------------------------------------

static void connectivity_defaults_to_online() {
  tth::ConversationStateMachine machine;
  TEST_ASSERT_EQUAL(tth::Connectivity::Online, machine.connectivity());
}

static void offline_start_up_lands_in_disconnected_and_refuses_presses() {
  tth::ConversationStateMachine machine;
  machine.setConnectivity(tth::Connectivity::Offline);
  TEST_ASSERT_TRUE(machine.markReady());
  TEST_ASSERT_EQUAL(tth::ConversationState::Disconnected, machine.state());
  TEST_ASSERT_FALSE(machine.onPress(100));
  TEST_ASSERT_EQUAL(tth::ConversationState::Disconnected, machine.state());
}

static void connecting_refuses_presses_too() {
  tth::ConversationStateMachine machine;
  machine.setConnectivity(tth::Connectivity::Connecting);
  machine.markReady();
  TEST_ASSERT_EQUAL(tth::ConversationState::Connecting, machine.state());
  TEST_ASSERT_FALSE(machine.onPress(100));
}

static void the_link_moves_an_idle_machine_both_ways() {
  tth::ConversationStateMachine machine = makeReadyMachine();
  TEST_ASSERT_TRUE(machine.setConnectivity(tth::Connectivity::Connecting));
  TEST_ASSERT_EQUAL(tth::ConversationState::Connecting, machine.state());
  TEST_ASSERT_TRUE(machine.setConnectivity(tth::Connectivity::Online));
  TEST_ASSERT_EQUAL(tth::ConversationState::Ready, machine.state());
  TEST_ASSERT_TRUE(machine.setConnectivity(tth::Connectivity::Offline));
  TEST_ASSERT_EQUAL(tth::ConversationState::Disconnected, machine.state());
  TEST_ASSERT_FALSE(machine.setConnectivity(tth::Connectivity::Offline));
  TEST_ASSERT_TRUE(machine.setConnectivity(tth::Connectivity::Online));
  TEST_ASSERT_TRUE(machine.onPress(200));
}

// A link change never interrupts a turn; the turn rests into the link's idle
// state when it ends.
static void a_link_change_never_interrupts_an_active_turn() {
  tth::ConversationStateMachine machine = makeReadyMachine();
  machine.onPress(1000);
  TEST_ASSERT_FALSE(machine.setConnectivity(tth::Connectivity::Offline));
  TEST_ASSERT_EQUAL(tth::ConversationState::Listening, machine.state());
  machine.onRelease(1500);
  TEST_ASSERT_TRUE(machine.onSpeechStart());
  TEST_ASSERT_FALSE(machine.setConnectivity(tth::Connectivity::Connecting));
  TEST_ASSERT_EQUAL(tth::ConversationState::Speaking, machine.state());
  TEST_ASSERT_TRUE(machine.onTurnComplete());
  TEST_ASSERT_EQUAL(tth::ConversationState::Connecting, machine.state());
}

static void barge_in_is_refused_while_not_online() {
  tth::ConversationStateMachine machine = makeWaitingMachine();
  machine.onSpeechStart();
  machine.setConnectivity(tth::Connectivity::Offline);
  TEST_ASSERT_FALSE(machine.onPress(3000));
  TEST_ASSERT_EQUAL(tth::ConversationState::Speaking, machine.state());
}

static void an_error_stays_through_link_changes_and_retries_only_online() {
  tth::ConversationStateMachine machine = makeReadyMachine();
  machine.onError();
  TEST_ASSERT_FALSE(machine.setConnectivity(tth::Connectivity::Offline));
  TEST_ASSERT_EQUAL(tth::ConversationState::Error, machine.state());
  TEST_ASSERT_FALSE(machine.onPress(100));
  machine.setConnectivity(tth::Connectivity::Online);
  TEST_ASSERT_EQUAL(tth::ConversationState::Error, machine.state());
  TEST_ASSERT_TRUE(machine.onPress(200));
}

// --- Step 6.2: a gateway that refuses the robot (D4) -------------------------

static void a_failed_link_is_an_error_that_refuses_presses_until_online() {
  tth::ConversationStateMachine machine = makeReadyMachine();
  TEST_ASSERT_TRUE(machine.setConnectivity(tth::Connectivity::Failed));
  TEST_ASSERT_EQUAL(tth::ConversationState::Error, machine.state());
  TEST_ASSERT_TRUE(machine.errorFromLink());
  TEST_ASSERT_FALSE(machine.onPress(100));
  TEST_ASSERT_EQUAL(tth::ConversationState::Error, machine.state());
  TEST_ASSERT_TRUE(machine.setConnectivity(tth::Connectivity::Online));
  TEST_ASSERT_EQUAL(tth::ConversationState::Ready, machine.state());
  TEST_ASSERT_FALSE(machine.errorFromLink());
}

static void starting_up_rejected_shows_the_error_face() {
  tth::ConversationStateMachine machine;
  machine.setConnectivity(tth::Connectivity::Failed);
  TEST_ASSERT_TRUE(machine.markReady());
  TEST_ASSERT_EQUAL(tth::ConversationState::Error, machine.state());
  TEST_ASSERT_TRUE(machine.setConnectivity(tth::Connectivity::Offline));
  TEST_ASSERT_EQUAL(tth::ConversationState::Disconnected, machine.state());
}

static void a_local_error_is_not_cleared_by_the_link() {
  tth::ConversationStateMachine machine = makeReadyMachine();
  machine.onError();
  TEST_ASSERT_FALSE(machine.errorFromLink());
  TEST_ASSERT_FALSE(machine.setConnectivity(tth::Connectivity::Failed));
  TEST_ASSERT_FALSE(machine.setConnectivity(tth::Connectivity::Online));
  TEST_ASSERT_EQUAL(tth::ConversationState::Error, machine.state());
}

static void a_turn_ending_while_rejected_rests_in_error() {
  tth::ConversationStateMachine machine = makeWaitingMachine();
  machine.setConnectivity(tth::Connectivity::Failed);
  TEST_ASSERT_TRUE(machine.onTurnComplete());
  TEST_ASSERT_EQUAL(tth::ConversationState::Error, machine.state());
  TEST_ASSERT_TRUE(machine.errorFromLink());
}

// --- Step 6.3: transient errors (D4) --------------------------------------------

static void a_transient_error_holds_then_rests_by_the_link() {
  tth::ConversationStateMachine machine = makeWaitingMachine();
  TEST_ASSERT_TRUE(machine.onTransientError(10000, 3000));
  TEST_ASSERT_EQUAL(tth::ConversationState::Error, machine.state());
  TEST_ASSERT_TRUE(machine.transientError());
  TEST_ASSERT_FALSE(machine.tick(12999));
  TEST_ASSERT_EQUAL(tth::ConversationState::Error, machine.state());
  TEST_ASSERT_TRUE(machine.tick(13000));
  TEST_ASSERT_EQUAL(tth::ConversationState::Ready, machine.state());
  TEST_ASSERT_FALSE(machine.tick(20000));
}

// Session lost mid-turn: Error for the hold, then Sleeping while offline, then
// Ready once the gateway is back -- never stuck in Error.
static void a_transient_error_while_offline_rests_sleeping_then_recovers() {
  tth::ConversationStateMachine machine = makeReadyMachine();
  machine.onPress(1000);
  machine.onTransientError(2000, 3000);
  TEST_ASSERT_FALSE(machine.setConnectivity(tth::Connectivity::Offline));
  TEST_ASSERT_EQUAL(tth::ConversationState::Error, machine.state());
  TEST_ASSERT_TRUE(machine.tick(5000));
  TEST_ASSERT_EQUAL(tth::ConversationState::Disconnected, machine.state());
  TEST_ASSERT_TRUE(machine.setConnectivity(tth::Connectivity::Online));
  TEST_ASSERT_EQUAL(tth::ConversationState::Ready, machine.state());
  TEST_ASSERT_TRUE(machine.onPress(6000));
}

static void a_press_during_a_transient_error_retries_at_once() {
  tth::ConversationStateMachine machine = makeReadyMachine();
  machine.onTransientError(1000, 3000);
  TEST_ASSERT_TRUE(machine.onPress(1500));
  TEST_ASSERT_EQUAL(tth::ConversationState::Listening, machine.state());
  TEST_ASSERT_FALSE(machine.tick(5000));
  TEST_ASSERT_EQUAL(tth::ConversationState::Listening, machine.state());
}

static void a_persistent_error_is_not_ended_by_tick() {
  tth::ConversationStateMachine machine = makeReadyMachine();
  machine.onError();
  TEST_ASSERT_FALSE(machine.transientError());
  TEST_ASSERT_FALSE(machine.tick(1000000));
  TEST_ASSERT_EQUAL(tth::ConversationState::Error, machine.state());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(a_transient_error_holds_then_rests_by_the_link);
  RUN_TEST(a_transient_error_while_offline_rests_sleeping_then_recovers);
  RUN_TEST(a_press_during_a_transient_error_retries_at_once);
  RUN_TEST(a_persistent_error_is_not_ended_by_tick);
  RUN_TEST(a_failed_link_is_an_error_that_refuses_presses_until_online);
  RUN_TEST(starting_up_rejected_shows_the_error_face);
  RUN_TEST(a_local_error_is_not_cleared_by_the_link);
  RUN_TEST(a_turn_ending_while_rejected_rests_in_error);
  RUN_TEST(connectivity_defaults_to_online);
  RUN_TEST(offline_start_up_lands_in_disconnected_and_refuses_presses);
  RUN_TEST(connecting_refuses_presses_too);
  RUN_TEST(the_link_moves_an_idle_machine_both_ways);
  RUN_TEST(a_link_change_never_interrupts_an_active_turn);
  RUN_TEST(barge_in_is_refused_while_not_online);
  RUN_TEST(an_error_stays_through_link_changes_and_retries_only_online);
  RUN_TEST(starts_in_booting);
  RUN_TEST(mark_ready_leaves_booting_exactly_once);
  RUN_TEST(press_moves_ready_to_listening);
  RUN_TEST(release_moves_listening_to_waiting);
  RUN_TEST(waiting_does_not_end_on_its_own);
  RUN_TEST(speech_start_moves_waiting_to_speaking);
  RUN_TEST(turn_complete_moves_speaking_to_ready);
  RUN_TEST(turn_complete_without_speech_moves_waiting_to_ready);
  RUN_TEST(speech_start_outside_waiting_is_ignored);
  RUN_TEST(turn_complete_outside_a_response_is_ignored);
  RUN_TEST(press_while_speaking_is_barge_in);
  RUN_TEST(press_while_listening_is_ignored);
  RUN_TEST(press_while_waiting_is_ignored);
  RUN_TEST(release_without_a_press_is_ignored);
  RUN_TEST(release_while_waiting_is_ignored);
  RUN_TEST(a_forced_release_completes_the_turn_normally);
  RUN_TEST(error_is_reachable_from_every_state_and_a_press_retries);
  RUN_TEST(several_turns_run_back_to_back);
  RUN_TEST(a_turn_across_the_millis_rollover_behaves_normally);
  return UNITY_END();
}
