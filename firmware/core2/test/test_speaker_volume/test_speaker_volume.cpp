// Host-side tests for the speaker master volume and the M5Unified mixer
// attenuation reported for it.

#include <unity.h>

#include "tth/SpeakerVolume.h"

void setUp() {}
void tearDown() {}

namespace {
// Core2: speaker magnification 16, channel volume 255 (M5Unified defaults).
const uint32_t kMag = 16;
const uint32_t kChannel = 255;
}  // namespace

// 255 is both the maximum and a value that passes through unchanged.
static void the_maximum_is_255_and_255_is_selectable() {
  TEST_ASSERT_EQUAL_UINT8(255, tth::kSpeakerVolumeMax);
  TEST_ASSERT_EQUAL_UINT8(255, tth::clampSpeakerVolume(255));
  TEST_ASSERT_EQUAL_UINT8(255, tth::clampSpeakerVolume(256));
  TEST_ASSERT_EQUAL_UINT8(255, tth::clampSpeakerVolume(100000));
  TEST_ASSERT_EQUAL_UINT8(160, tth::clampSpeakerVolume(160));
  TEST_ASSERT_EQUAL_UINT8(0, tth::clampSpeakerVolume(0));
}

// The attenuation matches M5Unified's mixer: 16 * V^2 * 255^2 / 2^36.
static void the_path_gain_matches_the_m5unified_mixer() {
  TEST_ASSERT_FLOAT_WITHIN(0.001, 0.1395,
                           tth::speakerPathGain(kMag, 96, kChannel));
  TEST_ASSERT_FLOAT_WITHIN(0.05, -17.11,
                           tth::speakerPathGainDb(kMag, 96, kChannel));
  TEST_ASSERT_FLOAT_WITHIN(0.001, 0.3876,
                           tth::speakerPathGain(kMag, 160, kChannel));
  TEST_ASSERT_FLOAT_WITHIN(0.05, -8.23,
                           tth::speakerPathGainDb(kMag, 160, kChannel));
  TEST_ASSERT_FLOAT_WITHIN(0.001, 0.9843,
                           tth::speakerPathGain(kMag, 255, kChannel));
  TEST_ASSERT_FLOAT_WITHIN(0.02, -0.14,
                           tth::speakerPathGainDb(kMag, 255, kChannel));
  TEST_ASSERT_FLOAT_WITHIN(0.001, -120.0,
                           tth::speakerPathGainDb(kMag, 0, kChannel));
}

// At the maximum the mixer is (almost) unity and still never amplifies, so a
// full-scale sample reaches the output without hitting the mixer's clamp.
static void at_255_full_scale_passes_without_digital_clipping() {
  const double g = tth::speakerPathGain(kMag, 255, kChannel);
  TEST_ASSERT_TRUE(g < 1.0);
  TEST_ASSERT_TRUE(32768.0 * g < 32767.5);
  for (uint32_t v = 0; v < 255; ++v) {
    if (tth::speakerPathGain(kMag, v, kChannel) >= g) {
      TEST_FAIL_MESSAGE("a lower volume was not quieter than 255");
    }
  }
}

// Volume is SQUARED: doubling it quadruples the amplitude (+12 dB).
static void the_path_gain_is_squared_in_the_volume() {
  const double g96 = tth::speakerPathGain(kMag, 96, kChannel);
  const double g192 = tth::speakerPathGain(kMag, 192, kChannel);
  TEST_ASSERT_FLOAT_WITHIN(0.0001, 4.0, g192 / g96);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(the_maximum_is_255_and_255_is_selectable);
  RUN_TEST(the_path_gain_matches_the_m5unified_mixer);
  RUN_TEST(at_255_full_scale_passes_without_digital_clipping);
  RUN_TEST(the_path_gain_is_squared_in_the_volume);
  return UNITY_END();
}
