// Host-side tests for the Step 6.2 memory changes: deterministic sprite
// placement (no fallback) and low-water attribution.

#include <unity.h>

#include "tth/LowWaterTracker.h"
#include "tth/MemorySafety.h"
#include "tth/SpritePlacement.h"

using tth::LowWaterTracker;
using tth::SpriteMemory;

void setUp() {}
void tearDown() {}

namespace {
const uintptr_t kPsramAddress = tth::kEsp32SpiramLow + 0x1000u;
const uintptr_t kDramAddress = tth::kEsp32DramLow + 0x2000u;
const uintptr_t kIramAddress = tth::kEsp32IramLow + 0x100u;
}  // namespace

// --- placement --------------------------------------------------------------------------

static void a_psram_sprite_must_really_be_in_psram() {
  TEST_ASSERT_TRUE(tth::spriteInRequestedRegion(SpriteMemory::Psram, kPsramAddress));
  TEST_ASSERT_FALSE(tth::spriteInRequestedRegion(SpriteMemory::Psram, kDramAddress));
  TEST_ASSERT_FALSE(tth::spriteInRequestedRegion(SpriteMemory::Psram, kIramAddress));
  TEST_ASSERT_FALSE(tth::spriteInRequestedRegion(SpriteMemory::Psram, 0));
}

static void an_internal_sprite_must_really_be_in_dram() {
  TEST_ASSERT_TRUE(tth::spriteInRequestedRegion(SpriteMemory::InternalDram, kDramAddress));
  TEST_ASSERT_FALSE(tth::spriteInRequestedRegion(SpriteMemory::InternalDram, kPsramAddress));
  // IRAM is internal but word-access only: never a valid sprite buffer.
  TEST_ASSERT_FALSE(tth::spriteInRequestedRegion(SpriteMemory::InternalDram, kIramAddress));
  TEST_ASSERT_FALSE(tth::spriteInRequestedRegion(SpriteMemory::InternalDram, 0));
}

static void placement_names_are_stable() {
  TEST_ASSERT_EQUAL_STRING("PSRAM", tth::toString(SpriteMemory::Psram));
  TEST_ASSERT_EQUAL_STRING("internal DRAM", tth::toString(SpriteMemory::InternalDram));
}

// --- low-water attribution ----------------------------------------------------------------

static void the_first_observation_starts_without_a_report() {
  LowWaterTracker t(2048);
  TEST_ASSERT_FALSE(t.started());
  TEST_ASSERT_FALSE(t.observe(50000, "boot"));
  TEST_ASSERT_TRUE(t.started());
  TEST_ASSERT_EQUAL_UINT32(50000, t.reportedLowWater());
  TEST_ASSERT_EQUAL_UINT32(0, t.reports());
}

static void a_large_drop_is_reported_with_its_stage() {
  LowWaterTracker t(2048);
  t.reset(24348);
  TEST_ASSERT_FALSE(t.observe(24348, "network"));
  TEST_ASSERT_TRUE(t.observe(12140, "yield+M5.update"));
  TEST_ASSERT_EQUAL_UINT32(12208, t.lastDrop());
  TEST_ASSERT_EQUAL_STRING("yield+M5.update", t.stage());
  TEST_ASSERT_EQUAL_UINT32(12140, t.reportedLowWater());
  TEST_ASSERT_EQUAL_UINT32(1, t.reports());
}

static void small_decreases_accumulate_until_the_step() {
  LowWaterTracker t(2048);
  t.reset(40000);
  TEST_ASSERT_FALSE(t.observe(39500, "a"));
  TEST_ASSERT_FALSE(t.observe(38500, "b"));
  TEST_ASSERT_TRUE(t.observe(37900, "c"));  // 2100 below the last report
  TEST_ASSERT_EQUAL_STRING("c", t.stage());
  TEST_ASSERT_EQUAL_UINT32(2100, t.lastDrop());
  TEST_ASSERT_EQUAL_UINT32(3, t.decreases());
  TEST_ASSERT_FALSE(t.observe(37000, "d"));  // measured from the new report
}

static void a_rising_or_equal_figure_is_ignored() {
  LowWaterTracker t(1024);
  t.reset(30000);
  TEST_ASSERT_FALSE(t.observe(31000, "x"));
  TEST_ASSERT_FALSE(t.observe(30000, "x"));
  TEST_ASSERT_EQUAL_UINT32(30000, t.lowWater());
  TEST_ASSERT_EQUAL_UINT32(0, t.decreases());
}

static void a_zero_step_reports_every_decrease() {
  LowWaterTracker t(0);
  t.reset(1000);
  TEST_ASSERT_TRUE(t.observe(999, nullptr));
  TEST_ASSERT_EQUAL_STRING("", t.stage());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(a_psram_sprite_must_really_be_in_psram);
  RUN_TEST(an_internal_sprite_must_really_be_in_dram);
  RUN_TEST(placement_names_are_stable);
  RUN_TEST(the_first_observation_starts_without_a_report);
  RUN_TEST(a_large_drop_is_reported_with_its_stage);
  RUN_TEST(small_decreases_accumulate_until_the_step);
  RUN_TEST(a_rising_or_equal_figure_is_ignored);
  RUN_TEST(a_zero_step_reports_every_decrease);
  return UNITY_END();
}
