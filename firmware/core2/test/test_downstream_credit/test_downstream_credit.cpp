// Host-side tests for downstream (gateway -> device) credit accounting.
//
// No call here can wait: a frame beyond credit is a counted violation, never
// a blocked task.

#include <unity.h>

#include "tth/DownstreamCredit.h"

void setUp() {}
void tearDown() {}

namespace {

struct Lcg {
  uint32_t state;
  uint32_t below(uint32_t n) {
    state = state * 1664525u + 1013904223u;
    return (state >> 8) % n;
  }
};

}  // namespace

static void zero_credit_admits_nothing() {
  tth::DownstreamCredit credit(0);
  TEST_ASSERT_FALSE(credit.admit(2));
  TEST_ASSERT_EQUAL_UINT32(1, credit.violations());
  TEST_ASSERT_TRUE(credit.desynced());
  TEST_ASSERT_EQUAL_UINT32(0, credit.received());
}

static void partial_credit_admits_exactly_up_to_the_grant() {
  tth::DownstreamCredit credit(1000);
  TEST_ASSERT_TRUE(credit.admit(600));
  TEST_ASSERT_TRUE(credit.admit(400));
  TEST_ASSERT_FALSE(credit.admit(2));
  TEST_ASSERT_EQUAL_UINT32(1000, credit.outstanding());
  TEST_ASSERT_EQUAL_UINT32(1, credit.violations());
}

// Credit comes back in batches, and only for audio actually consumed.
static void consumed_audio_is_returned_in_batches() {
  tth::DownstreamCredit credit(10000, 3840);
  TEST_ASSERT_TRUE(credit.admit(5000));
  credit.consumed(3000);
  TEST_ASSERT_EQUAL_UINT32(0, credit.takeReturn(false));  // below the batch
  credit.consumed(1000);
  TEST_ASSERT_EQUAL_UINT32(4000, credit.takeReturn(false));
  TEST_ASSERT_EQUAL_UINT32(1000, credit.outstanding());
  TEST_ASSERT_EQUAL_UINT32(0, credit.takeReturn(false));
  // The returned credit is usable again: 9000 more fits.
  TEST_ASSERT_TRUE(credit.admit(9000));
  TEST_ASSERT_FALSE(credit.admit(2));
}

static void a_forced_return_sends_any_remainder() {
  tth::DownstreamCredit credit(10000, 3840);
  credit.admit(100);
  credit.consumed(100);
  TEST_ASSERT_EQUAL_UINT32(0, credit.takeReturn(false));
  TEST_ASSERT_EQUAL_UINT32(100, credit.takeReturn(true));
  TEST_ASSERT_EQUAL_UINT32(0, credit.takeReturn(true));
}

// Cancelling with audio still in flight: the stale frames are admitted (they
// were sent within credit), discarded, and their credit returned -- nothing
// leaks.
static void cancellation_with_outstanding_credit_leaks_nothing() {
  tth::DownstreamCredit credit(8000, 3840);
  TEST_ASSERT_TRUE(credit.admit(1920));  // played
  credit.consumed(1920);
  TEST_ASSERT_TRUE(credit.admit(1920));  // stale, after cancel
  TEST_ASSERT_TRUE(credit.admit(1920));  // stale
  credit.consumed(3840);                 // discarded
  TEST_ASSERT_EQUAL_UINT32(5760, credit.takeReturn(true));
  TEST_ASSERT_EQUAL_UINT32(0, credit.outstanding());
  TEST_ASSERT_EQUAL_UINT32(0, credit.inRing());
  TEST_ASSERT_TRUE(credit.admit(8000));  // the full grant is available again
}

// A peer ignoring credit: every excess frame is a counted violation. admit()
// just returns -- it never waits for space.
static void a_peer_exceeding_credit_is_counted_not_waited_for() {
  tth::DownstreamCredit credit(3840);
  TEST_ASSERT_TRUE(credit.admit(1920));
  TEST_ASSERT_TRUE(credit.admit(1920));
  for (int i = 0; i < 5; ++i) TEST_ASSERT_FALSE(credit.admit(1920));
  TEST_ASSERT_EQUAL_UINT32(5, credit.violations());
  TEST_ASSERT_TRUE(credit.desynced());
  TEST_ASSERT_EQUAL_UINT32(3840, credit.received());  // excess not counted in
}

static void consuming_more_than_received_is_clamped_and_counted() {
  tth::DownstreamCredit credit(1000);
  credit.admit(100);
  credit.consumed(500);
  TEST_ASSERT_EQUAL_UINT32(1, credit.overConsumed());
  TEST_ASSERT_EQUAL_UINT32(0, credit.inRing());
  TEST_ASSERT_EQUAL_UINT32(100, credit.takeReturn(true));
}

static void a_reset_starts_a_fresh_grant() {
  tth::DownstreamCredit credit(1000);
  credit.admit(1000);
  credit.admit(1);  // violation
  credit.reset();
  TEST_ASSERT_FALSE(credit.desynced());
  TEST_ASSERT_EQUAL_UINT32(0, credit.outstanding());
  TEST_ASSERT_TRUE(credit.admit(1000));
}

// Totals wrap past 2^32 without breaking the arithmetic.
static void totals_wrap_without_breaking_the_accounting() {
  tth::DownstreamCredit credit(4000000000u, 1);
  TEST_ASSERT_TRUE(credit.admit(3000000000u));
  credit.consumed(3000000000u);
  TEST_ASSERT_EQUAL_UINT32(3000000000u, credit.takeReturn(true));
  TEST_ASSERT_TRUE(credit.admit(3000000000u));  // received wraps here
  TEST_ASSERT_EQUAL_UINT32(3000000000u, credit.outstanding());
  TEST_ASSERT_FALSE(credit.admit(1000000001u));
}

// An honest gateway that only sends within the credit it has been told about
// never triggers a violation, whatever the timing of consumption and returns;
// the conservation invariant holds throughout.
static void an_honest_peer_never_violates_and_credit_is_conserved() {
  const uint32_t capacity = 192000;
  tth::DownstreamCredit credit(capacity, 3840);
  Lcg rng = {99};
  uint64_t gatewaySent = 0;
  uint64_t gatewayToldReturned = 0;  // what the gateway has heard back
  uint64_t inRing = 0;

  for (int step = 0; step < 200000; ++step) {
    // Gateway: send if its own view of credit allows.
    const uint32_t frame = 2 * (1 + rng.below(960));
    const uint64_t gatewayCredit = capacity - (gatewaySent - gatewayToldReturned);
    if (frame <= gatewayCredit && rng.below(2) == 0) {
      TEST_ASSERT_TRUE(credit.admit(frame));
      gatewaySent += frame;
      inRing += frame;
    }
    // Device loop: consume some, return in batches; the gateway hears it
    // some time later (modelled by a delayed hand-over).
    if (inRing > 0 && rng.below(3) == 0) {
      const uint32_t take = static_cast<uint32_t>(
          (rng.below(4) == 0) ? inRing : (inRing < 1920 ? inRing : 1920));
      credit.consumed(take);
      inRing -= take;
    }
    static uint32_t inTransit = 0;
    inTransit += credit.takeReturn(rng.below(50) == 0);
    if (rng.below(4) == 0) {
      gatewayToldReturned += inTransit;
      inTransit = 0;
    }
    TEST_ASSERT_TRUE(credit.outstanding() <= capacity);
    TEST_ASSERT_TRUE(credit.inRing() <= capacity);
  }
  TEST_ASSERT_EQUAL_UINT32(0, credit.violations());
  TEST_ASSERT_FALSE(credit.desynced());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(zero_credit_admits_nothing);
  RUN_TEST(partial_credit_admits_exactly_up_to_the_grant);
  RUN_TEST(consumed_audio_is_returned_in_batches);
  RUN_TEST(a_forced_return_sends_any_remainder);
  RUN_TEST(cancellation_with_outstanding_credit_leaks_nothing);
  RUN_TEST(a_peer_exceeding_credit_is_counted_not_waited_for);
  RUN_TEST(consuming_more_than_received_is_clamped_and_counted);
  RUN_TEST(a_reset_starts_a_fresh_grant);
  RUN_TEST(totals_wrap_without_breaking_the_accounting);
  RUN_TEST(an_honest_peer_never_violates_and_credit_is_conserved);
  return UNITY_END();
}
