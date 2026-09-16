// Host-side tests for uint32 turn-id allocation: 0 is reserved, the counter
// wraps to 1, and a new id never collides with the active turn or a recently
// ended/cancelled one.

#include <unity.h>

#include "tth/TurnIdAllocator.h"

void setUp() {}
void tearDown() {}

static void ids_start_at_one_and_increment() {
  tth::TurnIdAllocator ids;
  TEST_ASSERT_EQUAL_UINT32(1, ids.allocate());
  TEST_ASSERT_EQUAL_UINT32(1, ids.active());
  TEST_ASSERT_EQUAL_UINT32(2, ids.allocate());
  TEST_ASSERT_EQUAL_UINT32(3, ids.allocate());
}

static void a_starting_value_of_zero_is_treated_as_one() {
  tth::TurnIdAllocator ids(0);
  TEST_ASSERT_EQUAL_UINT32(1, ids.allocate());
}

// 0xFFFFFFFF wraps to 1 -- never to the reserved 0.
static void the_counter_wraps_past_zero() {
  tth::TurnIdAllocator ids(0xFFFFFFFEu);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFEu, ids.allocate());
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, ids.allocate());
  TEST_ASSERT_EQUAL_UINT32(1, ids.allocate());
}

// Right at the wrap, ids that are still recent (a late frame could carry
// them) are skipped.
static void wraparound_skips_recently_cancelled_ids() {
  tth::TurnIdAllocator ids(0xFFFFFFFFu);
  ids.finish(1);
  ids.finish(2);
  ids.finish(3);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, ids.allocate());
  TEST_ASSERT_EQUAL_UINT32(4, ids.allocate());
}

static void a_new_id_never_equals_the_active_or_a_recent_one() {
  tth::TurnIdAllocator ids(0xFFFFFFF0u);
  uint32_t last = 0;
  for (int i = 0; i < 200000; ++i) {
    const uint32_t id = ids.allocate();
    if (id == 0) TEST_FAIL_MESSAGE("allocated the reserved id 0");
    if (id == last) TEST_FAIL_MESSAGE("reused the previous (active) id");
    last = id;
  }
}

static void the_recent_set_keeps_the_last_sixteen() {
  tth::TurnIdAllocator ids;
  for (uint32_t turn = 1; turn <= 17; ++turn) ids.finish(turn);
  TEST_ASSERT_FALSE(ids.isRecent(1));
  TEST_ASSERT_TRUE(ids.isRecent(2));
  TEST_ASSERT_TRUE(ids.isRecent(17));
  TEST_ASSERT_FALSE(ids.isRecent(0));
}

static void finishing_the_active_turn_clears_it() {
  tth::TurnIdAllocator ids;
  const uint32_t turn = ids.allocate();
  ids.finish(turn);
  TEST_ASSERT_EQUAL_UINT32(0, ids.active());
  TEST_ASSERT_TRUE(ids.isRecent(turn));
}

// Allocating while a turn is still active finishes it first, so it becomes
// recent and cannot be handed out again soon.
static void allocating_retires_the_previous_active_turn() {
  tth::TurnIdAllocator ids;
  const uint32_t first = ids.allocate();
  const uint32_t second = ids.allocate();
  TEST_ASSERT_TRUE(ids.isRecent(first));
  TEST_ASSERT_EQUAL_UINT32(second, ids.active());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(ids_start_at_one_and_increment);
  RUN_TEST(a_starting_value_of_zero_is_treated_as_one);
  RUN_TEST(the_counter_wraps_past_zero);
  RUN_TEST(wraparound_skips_recently_cancelled_ids);
  RUN_TEST(a_new_id_never_equals_the_active_or_a_recent_one);
  RUN_TEST(the_recent_set_keeps_the_last_sixteen);
  RUN_TEST(finishing_the_active_turn_clears_it);
  RUN_TEST(allocating_retires_the_previous_active_turn);
  return UNITY_END();
}
