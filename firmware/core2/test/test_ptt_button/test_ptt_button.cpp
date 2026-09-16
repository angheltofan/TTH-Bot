// Host-side tests for PttButton: debouncing, one-shot edges, hold duration
// and the maximum-hold safety timeout.
//
// PttButton takes time as a parameter rather than reading a clock, so a 45
// second timeout is tested by passing 45000 -- no waiting, no hardware.

#include <unity.h>

#include "tth/PttButton.h"

namespace {

const uint32_t kDebounceMs = 30;
const uint32_t kMaxHoldMs = 45000;

tth::PttButton makeButton() {
  return tth::PttButton(kDebounceMs, kMaxHoldMs);
}

// Feeds a steady level across a span of time, one sample per millisecond, the
// way the main loop would.
void hold(tth::PttButton& button, bool down, uint32_t fromMs, uint32_t toMs) {
  for (uint32_t t = fromMs; t <= toMs; ++t) {
    button.update(down, t);
  }
}

}  // namespace

void setUp() {}
void tearDown() {}

static void starts_idle_with_no_edges() {
  tth::PttButton button = makeButton();

  TEST_ASSERT_FALSE(button.isHeld());
  TEST_ASSERT_FALSE(button.consumePress());
  TEST_ASSERT_FALSE(button.consumeRelease());
  TEST_ASSERT_EQUAL_UINT32(0, button.heldForMs(0));
}

// --- debouncing ------------------------------------------------------------

static void press_is_not_believed_until_the_level_is_stable() {
  tth::PttButton button = makeButton();

  button.update(true, 1000);
  TEST_ASSERT_FALSE(button.isHeld());
  TEST_ASSERT_FALSE(button.consumePress());

  // One millisecond short of the debounce window.
  button.update(true, 1000 + kDebounceMs - 1);
  TEST_ASSERT_FALSE(button.isHeld());
  TEST_ASSERT_FALSE(button.consumePress());

  button.update(true, 1000 + kDebounceMs);
  TEST_ASSERT_TRUE(button.isHeld());
  TEST_ASSERT_TRUE(button.consumePress());
}

// Contact bounce: the level flaps for a few ms, then settles. Exactly one
// press must be reported, and only once the flapping has stopped.
static void contact_bounce_produces_exactly_one_press() {
  tth::PttButton button = makeButton();
  uint32_t t = 5000;

  for (int i = 0; i < 6; ++i) {
    button.update(true, t++);
    button.update(false, t++);
  }
  TEST_ASSERT_FALSE(button.isHeld());
  TEST_ASSERT_FALSE(button.consumePress());

  hold(button, true, t, t + kDebounceMs);
  TEST_ASSERT_TRUE(button.isHeld());
  TEST_ASSERT_TRUE(button.consumePress());
  TEST_ASSERT_FALSE(button.consumePress());
}

static void release_bounce_produces_exactly_one_release() {
  tth::PttButton button = makeButton();
  hold(button, true, 0, kDebounceMs);
  button.consumePress();

  uint32_t t = 2000;
  for (int i = 0; i < 6; ++i) {
    button.update(false, t++);
    button.update(true, t++);
  }
  TEST_ASSERT_FALSE(button.consumeRelease());

  hold(button, false, t, t + kDebounceMs);
  TEST_ASSERT_FALSE(button.isHeld());
  TEST_ASSERT_TRUE(button.consumeRelease());
  TEST_ASSERT_FALSE(button.consumeRelease());
}

// A zero debounce window is what the capacitive touch input uses, since
// M5Unified has already debounced it.
static void zero_debounce_accepts_the_level_immediately() {
  tth::PttButton button(0, kMaxHoldMs);

  button.update(true, 100);
  TEST_ASSERT_TRUE(button.isHeld());
  TEST_ASSERT_TRUE(button.consumePress());

  button.update(false, 101);
  TEST_ASSERT_FALSE(button.isHeld());
  TEST_ASSERT_TRUE(button.consumeRelease());
}

// --- edges -----------------------------------------------------------------

static void edges_are_one_shot() {
  tth::PttButton button = makeButton();

  hold(button, true, 0, kDebounceMs);
  TEST_ASSERT_TRUE(button.consumePress());
  TEST_ASSERT_FALSE(button.consumePress());

  hold(button, true, kDebounceMs + 1, 500);
  TEST_ASSERT_FALSE(button.consumePress());
  TEST_ASSERT_TRUE(button.isHeld());
}

static void held_duration_tracks_the_press() {
  tth::PttButton button = makeButton();

  hold(button, true, 1000, 1000 + kDebounceMs);
  // The press is timestamped when the debounced level changed.
  const uint32_t pressedAt = 1000 + kDebounceMs;
  TEST_ASSERT_EQUAL_UINT32(0, button.heldForMs(pressedAt));
  TEST_ASSERT_EQUAL_UINT32(500, button.heldForMs(pressedAt + 500));
}

static void held_duration_is_zero_when_not_held() {
  tth::PttButton button = makeButton();
  TEST_ASSERT_EQUAL_UINT32(0, button.heldForMs(9999));
}

// The duration must survive until the caller gets round to consuming the
// release edge, which may be a later loop iteration.
static void last_hold_duration_is_captured_at_the_release() {
  tth::PttButton button = makeButton();

  hold(button, true, 0, kDebounceMs);
  button.consumePress();
  const uint32_t pressedAt = kDebounceMs;

  hold(button, true, pressedAt + 1, pressedAt + 2000);
  hold(button, false, pressedAt + 2001, pressedAt + 2001 + kDebounceMs);

  const uint32_t releasedAt = pressedAt + 2001 + kDebounceMs;
  TEST_ASSERT_TRUE(button.consumeRelease());
  TEST_ASSERT_EQUAL_UINT32(releasedAt - pressedAt, button.lastHoldMs());

  // Still available several iterations later.
  hold(button, false, releasedAt + 1, releasedAt + 100);
  TEST_ASSERT_EQUAL_UINT32(releasedAt - pressedAt, button.lastHoldMs());
}

static void a_normal_release_is_not_marked_forced() {
  tth::PttButton button = makeButton();

  hold(button, true, 0, kDebounceMs);
  button.consumePress();
  hold(button, false, 1000, 1000 + kDebounceMs);

  TEST_ASSERT_TRUE(button.consumeRelease());
  TEST_ASSERT_FALSE(button.lastReleaseWasForced());
}

// --- maximum-hold timeout --------------------------------------------------

static void hold_beyond_the_ceiling_forces_a_release() {
  tth::PttButton button = makeButton();

  hold(button, true, 0, kDebounceMs);
  TEST_ASSERT_TRUE(button.consumePress());
  const uint32_t pressedAt = kDebounceMs;

  // Just under the ceiling: still held.
  button.update(true, pressedAt + kMaxHoldMs - 1);
  TEST_ASSERT_TRUE(button.isHeld());
  TEST_ASSERT_FALSE(button.consumeRelease());

  // At the ceiling: released, even though the button is still down.
  button.update(true, pressedAt + kMaxHoldMs);
  TEST_ASSERT_FALSE(button.isHeld());
  TEST_ASSERT_TRUE(button.consumeRelease());
  TEST_ASSERT_TRUE(button.lastReleaseWasForced());
  TEST_ASSERT_EQUAL_UINT32(kMaxHoldMs, button.lastHoldMs());
}

// A stuck button must not re-arm every 45 seconds. It must be genuinely
// released first.
static void a_stuck_button_does_not_re_trigger() {
  tth::PttButton button = makeButton();

  hold(button, true, 0, kDebounceMs);
  button.consumePress();
  button.update(true, kDebounceMs + kMaxHoldMs);
  TEST_ASSERT_TRUE(button.consumeRelease());

  // Still physically down, far past another whole ceiling's worth of time.
  for (uint32_t t = kDebounceMs + kMaxHoldMs + 1;
       t <= kDebounceMs + (3 * kMaxHoldMs); t += 97) {
    button.update(true, t);
  }
  TEST_ASSERT_FALSE(button.isHeld());
  TEST_ASSERT_FALSE(button.consumePress());
  TEST_ASSERT_FALSE(button.consumeRelease());
}

// ...and letting go after a forced release must not look like a second
// release event.
static void releasing_after_a_forced_release_emits_nothing() {
  tth::PttButton button = makeButton();

  hold(button, true, 0, kDebounceMs);
  button.consumePress();
  button.update(true, kDebounceMs + kMaxHoldMs);
  TEST_ASSERT_TRUE(button.consumeRelease());

  uint32_t t = kDebounceMs + kMaxHoldMs + 1;
  hold(button, false, t, t + (2 * kDebounceMs));
  TEST_ASSERT_FALSE(button.consumeRelease());
  TEST_ASSERT_FALSE(button.consumePress());
}

// After genuinely letting go, the next press works normally again.
static void a_fresh_press_works_after_a_forced_release() {
  tth::PttButton button = makeButton();

  hold(button, true, 0, kDebounceMs);
  button.consumePress();
  button.update(true, kDebounceMs + kMaxHoldMs);
  button.consumeRelease();

  uint32_t t = kDebounceMs + kMaxHoldMs + 1;
  hold(button, false, t, t + (2 * kDebounceMs));
  t += 2 * kDebounceMs;

  hold(button, true, t + 1, t + 1 + kDebounceMs);
  TEST_ASSERT_TRUE(button.isHeld());
  TEST_ASSERT_TRUE(button.consumePress());
  TEST_ASSERT_FALSE(button.lastReleaseWasForced());
}

static void reset_clears_everything_without_emitting_events() {
  tth::PttButton button = makeButton();

  hold(button, true, 0, kDebounceMs);
  button.reset();

  TEST_ASSERT_FALSE(button.isHeld());
  TEST_ASSERT_FALSE(button.consumePress());
  TEST_ASSERT_FALSE(button.consumeRelease());
  TEST_ASSERT_EQUAL_UINT32(0, button.lastHoldMs());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(starts_idle_with_no_edges);
  RUN_TEST(press_is_not_believed_until_the_level_is_stable);
  RUN_TEST(contact_bounce_produces_exactly_one_press);
  RUN_TEST(release_bounce_produces_exactly_one_release);
  RUN_TEST(zero_debounce_accepts_the_level_immediately);
  RUN_TEST(edges_are_one_shot);
  RUN_TEST(held_duration_tracks_the_press);
  RUN_TEST(held_duration_is_zero_when_not_held);
  RUN_TEST(last_hold_duration_is_captured_at_the_release);
  RUN_TEST(a_normal_release_is_not_marked_forced);
  RUN_TEST(hold_beyond_the_ceiling_forces_a_release);
  RUN_TEST(a_stuck_button_does_not_re_trigger);
  RUN_TEST(releasing_after_a_forced_release_emits_nothing);
  RUN_TEST(a_fresh_press_works_after_a_forced_release);
  RUN_TEST(reset_clears_everything_without_emitting_events);
  return UNITY_END();
}
