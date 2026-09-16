// Host-side tests for the Wi-Fi LinkStateMachine and its backoff.

#include <unity.h>

#include "tth/LinkStateMachine.h"

using tth::Connectivity;
using tth::LinkAction;
using tth::LinkFailure;
using tth::LinkState;
using tth::LinkStateMachine;

void setUp() {}
void tearDown() {}

namespace {
const uint32_t kTimeout = 15000;

void assertWithinJitter(uint32_t baseMs, uint32_t actualMs) {
  TEST_ASSERT_TRUE_MESSAGE(actualMs >= (baseMs * 8u) / 10u, "below -20 %");
  TEST_ASSERT_TRUE_MESSAGE(actualMs <= (baseMs * 12u) / 10u, "above +20 %");
}
}  // namespace

static void unprovisioned_does_nothing_and_is_offline() {
  LinkStateMachine m(kTimeout, 42);
  TEST_ASSERT_TRUE(m.start(false, 0) == LinkAction::None);
  TEST_ASSERT_TRUE(m.state() == LinkState::Unprovisioned);
  TEST_ASSERT_TRUE(m.connectivity() == Connectivity::Offline);
  for (uint32_t t = 0; t < 100000; t += 1000) {
    TEST_ASSERT_TRUE(m.poll(t, false) == LinkAction::None);
  }
}

static void provisioned_connects_and_goes_online() {
  LinkStateMachine m(kTimeout, 42);
  TEST_ASSERT_TRUE(m.start(true, 100) == LinkAction::StartConnect);
  TEST_ASSERT_TRUE(m.connectivity() == Connectivity::Connecting);
  TEST_ASSERT_TRUE(m.poll(2000, false) == LinkAction::None);
  TEST_ASSERT_TRUE(m.poll(3000, true) == LinkAction::None);
  TEST_ASSERT_TRUE(m.state() == LinkState::Connected);
  TEST_ASSERT_TRUE(m.connectivity() == Connectivity::Online);
  TEST_ASSERT_EQUAL_UINT32(1, m.connects());
  TEST_ASSERT_EQUAL_UINT32(0, m.attempt());
}

// PHASE6_PLAN §8: 1, 2, 4, 8, 16, then 30 s, each ±20 %.
static void repeated_timeouts_back_off_1_2_4_8_16_30_30() {
  LinkStateMachine m(kTimeout, 7);
  uint32_t now = 0;
  m.start(true, now);
  const uint32_t expected[8] = {1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000};
  for (int i = 0; i < 8; ++i) {
    now += kTimeout - 1;
    TEST_ASSERT_TRUE(m.poll(now, false) == LinkAction::None);
    now += 1;
    TEST_ASSERT_TRUE(m.poll(now, false) == LinkAction::Disconnect);
    TEST_ASSERT_TRUE(m.state() == LinkState::Backoff);
    TEST_ASSERT_TRUE(m.lastFailure() == LinkFailure::ConnectTimeout);
    TEST_ASSERT_TRUE(m.connectivity() == Connectivity::Offline);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(i + 1), m.attempt());
    assertWithinJitter(expected[i], m.lastDelayMs());

    // Not a millisecond early.
    now += m.lastDelayMs() - 1;
    TEST_ASSERT_TRUE(m.poll(now, false) == LinkAction::None);
    now += 1;
    TEST_ASSERT_TRUE(m.poll(now, false) == LinkAction::StartConnect);
    TEST_ASSERT_TRUE(m.state() == LinkState::Connecting);
  }
}

static void a_lost_link_retries_from_the_first_step() {
  LinkStateMachine m(kTimeout, 9);
  m.start(true, 0);
  // Two failures first, so the reset is visible.
  m.poll(kTimeout, false);
  m.poll(kTimeout + m.lastDelayMs(), false);
  const uint32_t t = kTimeout + m.lastDelayMs() + kTimeout;
  m.poll(t, false);
  m.poll(t + m.lastDelayMs(), false);
  TEST_ASSERT_EQUAL_UINT32(2, m.attempt());

  m.poll(t + m.lastDelayMs() + 500, true);
  TEST_ASSERT_TRUE(m.state() == LinkState::Connected);
  TEST_ASSERT_EQUAL_UINT32(0, m.attempt());

  TEST_ASSERT_TRUE(m.poll(200000, false) == LinkAction::Disconnect);
  TEST_ASSERT_TRUE(m.lastFailure() == LinkFailure::LinkLost);
  TEST_ASSERT_EQUAL_UINT32(1, m.attempt());
  assertWithinJitter(1000, m.lastDelayMs());

  TEST_ASSERT_TRUE(m.poll(200000 + m.lastDelayMs(), false) == LinkAction::StartConnect);
  m.poll(200000 + m.lastDelayMs() + 100, true);
  TEST_ASSERT_EQUAL_UINT32(2, m.connects());
}

static void unprovisioning_stops_the_link() {
  LinkStateMachine m(kTimeout, 1);
  m.start(true, 0);
  m.poll(10, true);
  TEST_ASSERT_TRUE(m.start(false, 20) == LinkAction::Disconnect);
  TEST_ASSERT_TRUE(m.state() == LinkState::Unprovisioned);
  TEST_ASSERT_TRUE(m.poll(30, true) == LinkAction::None);
  TEST_ASSERT_TRUE(m.connectivity() == Connectivity::Offline);
}

// New credentials during a long backoff apply at once, from attempt 0.
static void reprovisioning_during_backoff_connects_immediately() {
  LinkStateMachine m(kTimeout, 3);
  uint32_t now = 0;
  m.start(true, now);
  for (int i = 0; i < 6; ++i) {
    now += kTimeout;
    m.poll(now, false);
    now += m.lastDelayMs();
    m.poll(now, false);
  }
  now += kTimeout;
  m.poll(now, false);
  TEST_ASSERT_TRUE(m.state() == LinkState::Backoff);
  TEST_ASSERT_TRUE(m.start(true, now + 5) == LinkAction::StartConnect);
  TEST_ASSERT_EQUAL_UINT32(0, m.attempt());
  TEST_ASSERT_TRUE(m.state() == LinkState::Connecting);
}

static void jitter_is_deterministic_per_seed_and_varies() {
  tth::Backoff a(1234);
  tth::Backoff b(1234);
  tth::Backoff c(99);
  bool differs = false;
  uint32_t first = 0;
  bool spread = false;
  for (uint32_t i = 0; i < 50; ++i) {
    const uint32_t da = a.delayMs(5);
    TEST_ASSERT_EQUAL_UINT32(da, b.delayMs(5));
    if (c.delayMs(5) != da) differs = true;
    if (i == 0) first = da;
    if (da != first) spread = true;
    assertWithinJitter(30000, da);
  }
  TEST_ASSERT_TRUE(differs);
  TEST_ASSERT_TRUE(spread);
  tth::Backoff zero(0);  // a zero seed must not stall the generator
  const uint32_t z1 = zero.delayMs(0);
  bool zeroSpread = false;
  for (int i = 0; i < 20; ++i) {
    if (zero.delayMs(0) != z1) zeroSpread = true;
  }
  TEST_ASSERT_TRUE(zeroSpread);
}

static void timing_survives_millis_rollover() {
  LinkStateMachine m(kTimeout, 5);
  const uint32_t start = 0xFFFFF000u;  // 4.1 s before the wrap
  m.start(true, start);
  TEST_ASSERT_TRUE(m.poll(start + kTimeout - 1, false) == LinkAction::None);
  const uint32_t failAt = start + kTimeout;  // wrapped
  TEST_ASSERT_TRUE(m.poll(failAt, false) == LinkAction::Disconnect);
  TEST_ASSERT_TRUE(m.poll(failAt + m.lastDelayMs() - 1, false) == LinkAction::None);
  TEST_ASSERT_TRUE(m.poll(failAt + m.lastDelayMs(), false) == LinkAction::StartConnect);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(unprovisioned_does_nothing_and_is_offline);
  RUN_TEST(provisioned_connects_and_goes_online);
  RUN_TEST(repeated_timeouts_back_off_1_2_4_8_16_30_30);
  RUN_TEST(a_lost_link_retries_from_the_first_step);
  RUN_TEST(unprovisioning_stops_the_link);
  RUN_TEST(reprovisioning_during_backoff_connects_immediately);
  RUN_TEST(jitter_is_deterministic_per_seed_and_varies);
  RUN_TEST(timing_survives_millis_rollover);
  return UNITY_END();
}
