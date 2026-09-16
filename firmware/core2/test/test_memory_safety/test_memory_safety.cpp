// Host-side regression tests for the Phase 4 crash.
//
// THE BUG
//
// A 1 KB microphone chunk buffer was allocated with
// heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL). That cap only means "not
// SPIRAM" -- it can return IRAM, and it did, at 0x40092464. M5Unified's
// microphone task writes int16 samples straight into that buffer
// (Mic_Class.cpp:743). ESP32 IRAM permits only 32-bit aligned word access, so
// the very first 16-bit store raised a LoadStoreError (EXCCAUSE 3) and
// panicked core 0 on every single push-to-talk press.
//
// These tests pin the address-range predicates the firmware now checks every
// buffer against at start-up, using the exact address from the crash report.

#include <unity.h>

#include "tth/MemorySafety.h"

void setUp() {}
void tearDown() {}

// The literal address from the panic. If this ever reports "safe", the guard
// that would have caught the crash is broken.
static void the_address_from_the_crash_is_rejected() {
  const uintptr_t crashAddress = 0x40092464u;

  TEST_ASSERT_FALSE(tth::isByteAddressable(crashAddress));
  TEST_ASSERT_FALSE(tth::isDmaCapable(crashAddress));
  TEST_ASSERT_EQUAL_STRING("IRAM", tth::memoryRegionName(crashAddress));
}

static void iram_is_never_byte_addressable() {
  const uintptr_t addresses[4] = {tth::kEsp32IramLow, 0x40085000u, 0x40092464u,
                                  tth::kEsp32IramHigh - 4u};
  for (int i = 0; i < 4; ++i) {
    TEST_ASSERT_FALSE(tth::isByteAddressable(addresses[i]));
    TEST_ASSERT_FALSE(tth::isDmaCapable(addresses[i]));
  }
}

static void dram_is_byte_addressable_and_dma_capable() {
  const uintptr_t addresses[3] = {tth::kEsp32DramLow, 0x3FFC1234u,
                                  tth::kEsp32DramHigh - 4u};
  for (int i = 0; i < 3; ++i) {
    TEST_ASSERT_TRUE(tth::isByteAddressable(addresses[i]));
    TEST_ASSERT_TRUE(tth::isDmaCapable(addresses[i]));
    TEST_ASSERT_EQUAL_STRING("DRAM", tth::memoryRegionName(addresses[i]));
  }
}

// PSRAM holds the 1.4 MB turn buffer, so it must pass the byte-addressable
// check -- but it must never be accepted as a DMA destination.
static void psram_is_byte_addressable_but_not_dma_capable() {
  const uintptr_t addresses[3] = {tth::kEsp32SpiramLow, 0x3F900000u,
                                  tth::kEsp32SpiramHigh - 4u};
  for (int i = 0; i < 3; ++i) {
    TEST_ASSERT_TRUE(tth::isByteAddressable(addresses[i]));
    TEST_ASSERT_FALSE(tth::isDmaCapable(addresses[i]));
    TEST_ASSERT_EQUAL_STRING("PSRAM", tth::memoryRegionName(addresses[i]));
  }
}

static void a_null_pointer_is_never_safe() {
  TEST_ASSERT_FALSE(tth::isByteAddressable(0));
  TEST_ASSERT_FALSE(tth::isDmaCapable(0));
  TEST_ASSERT_FALSE(tth::isPsram(0));
  TEST_ASSERT_EQUAL_STRING("null", tth::memoryRegionName(0));
}

// Flash, ROM and peripheral addresses are not data RAM either.
static void addresses_outside_every_ram_region_are_rejected() {
  const uintptr_t addresses[3] = {0x00000000u, 0x3F400000u, 0x400C0000u};
  for (int i = 0; i < 3; ++i) {
    TEST_ASSERT_FALSE(tth::isByteAddressable(addresses[i]));
    TEST_ASSERT_FALSE(tth::isDmaCapable(addresses[i]));
  }
}

// The boundaries must be half-open, so the region above never leaks in.
static void the_region_boundaries_are_exact() {
  TEST_ASSERT_TRUE(tth::isDmaCapable(tth::kEsp32DramLow));
  TEST_ASSERT_FALSE(tth::isDmaCapable(tth::kEsp32DramLow - 1u));
  TEST_ASSERT_FALSE(tth::isDmaCapable(tth::kEsp32DramHigh));

  TEST_ASSERT_TRUE(tth::isPsram(tth::kEsp32SpiramLow));
  TEST_ASSERT_FALSE(tth::isPsram(tth::kEsp32SpiramLow - 1u));
  TEST_ASSERT_FALSE(tth::isPsram(tth::kEsp32SpiramHigh));

  // One byte below IRAM is still not valid RAM on this chip.
  TEST_ASSERT_FALSE(tth::isByteAddressable(tth::kEsp32IramLow));
  TEST_ASSERT_FALSE(tth::isByteAddressable(tth::kEsp32IramHigh - 1u));
}

// The regions must not overlap, or a buffer could satisfy two contradictory
// checks at once.
static void the_regions_do_not_overlap() {
  TEST_ASSERT_TRUE(tth::kEsp32SpiramHigh <= tth::kEsp32DramLow);
  TEST_ASSERT_TRUE(tth::kEsp32DramHigh <= tth::kEsp32IramLow);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(the_address_from_the_crash_is_rejected);
  RUN_TEST(iram_is_never_byte_addressable);
  RUN_TEST(dram_is_byte_addressable_and_dma_capable);
  RUN_TEST(psram_is_byte_addressable_but_not_dma_capable);
  RUN_TEST(a_null_pointer_is_never_safe);
  RUN_TEST(addresses_outside_every_ram_region_are_rejected);
  RUN_TEST(the_region_boundaries_are_exact);
  RUN_TEST(the_regions_do_not_overlap);
  return UNITY_END();
}
