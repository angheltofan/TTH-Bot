// Host-side tests for the preallocated turn buffer and the per-chunk PCM
// processing.

#include <string.h>

#include <unity.h>

#include "tth/PcmProcessing.h"
#include "tth/TurnBuffer.h"

namespace {

const uint32_t kCapacity = 100;
int16_t g_storage[kCapacity];

void attach(tth::TurnBuffer& buffer) {
  memset(g_storage, 0, sizeof(g_storage));
  buffer.attach(g_storage, kCapacity);
}

void fill(int16_t* dest, uint32_t count, int16_t value) {
  for (uint32_t i = 0; i < count; ++i) dest[i] = value;
}

}  // namespace

void setUp() {}
void tearDown() {}

// --- buffer basics ----------------------------------------------------------

static void an_unattached_buffer_accepts_nothing() {
  tth::TurnBuffer buffer;
  int16_t chunk[4] = {1, 2, 3, 4};

  TEST_ASSERT_FALSE(buffer.isAttached());
  TEST_ASSERT_EQUAL_UINT32(0, buffer.append(chunk, 4));
  TEST_ASSERT_EQUAL_UINT32(0, buffer.size());
  TEST_ASSERT_EQUAL_UINT32(0, buffer.capacity());
}

static void appending_stores_the_samples_in_order() {
  tth::TurnBuffer buffer;
  attach(buffer);
  const int16_t first[3] = {10, 20, 30};
  const int16_t second[2] = {40, 50};

  TEST_ASSERT_EQUAL_UINT32(3, buffer.append(first, 3));
  TEST_ASSERT_EQUAL_UINT32(2, buffer.append(second, 2));
  TEST_ASSERT_EQUAL_UINT32(5, buffer.size());
  TEST_ASSERT_EQUAL_UINT32(10, buffer.byteSize());

  const int16_t expected[5] = {10, 20, 30, 40, 50};
  for (int i = 0; i < 5; ++i) {
    TEST_ASSERT_EQUAL_INT16(expected[i], buffer.data()[i]);
  }
}

// A chunk that lands exactly on the capacity must be stored WHOLE -- no
// partial write, nothing dropped -- and leave the buffer full.
static void a_chunk_that_exactly_fills_the_buffer_is_stored_whole() {
  tth::TurnBuffer buffer;
  attach(buffer);
  int16_t chunk[kCapacity];
  fill(chunk, kCapacity, 7);

  TEST_ASSERT_EQUAL_UINT32(kCapacity, buffer.append(chunk, kCapacity));
  TEST_ASSERT_TRUE(buffer.isFull());
  TEST_ASSERT_EQUAL_UINT32(0, buffer.remaining());
  TEST_ASSERT_EQUAL_UINT32(kCapacity, buffer.size());
}

// An oversized chunk must fill exactly to the brim and no further. The
// shortfall is the caller's to report.
static void an_oversized_chunk_is_truncated_at_the_capacity() {
  tth::TurnBuffer buffer;
  attach(buffer);
  int16_t chunk[kCapacity + 20];
  fill(chunk, kCapacity + 20, 5);

  TEST_ASSERT_EQUAL_UINT32(kCapacity, buffer.append(chunk, kCapacity + 20));
  TEST_ASSERT_EQUAL_UINT32(kCapacity, buffer.size());
  TEST_ASSERT_TRUE(buffer.isFull());
}

// The regression guard for the whole design: nothing may ever be written past
// the end of the storage.
static void nothing_is_ever_written_past_the_end() {
  const uint32_t kGuarded = 16;
  int16_t region[kGuarded + 8];
  // Sentinels either side of the usable area.
  for (uint32_t i = 0; i < kGuarded + 8; ++i) region[i] = -12345;

  tth::TurnBuffer buffer;
  buffer.attach(region + 4, kGuarded);

  int16_t chunk[64];
  fill(chunk, 64, 99);
  for (int attempt = 0; attempt < 10; ++attempt) {
    buffer.append(chunk, 64);
  }

  for (int i = 0; i < 4; ++i) {
    TEST_ASSERT_EQUAL_INT16(-12345, region[i]);
    TEST_ASSERT_EQUAL_INT16(-12345, region[kGuarded + 4 + i]);
  }
  TEST_ASSERT_EQUAL_UINT32(kGuarded, buffer.size());
}

static void a_full_buffer_accepts_nothing_more() {
  tth::TurnBuffer buffer;
  attach(buffer);
  int16_t chunk[kCapacity];
  fill(chunk, kCapacity, 1);
  buffer.append(chunk, kCapacity);

  const int16_t more[4] = {2, 2, 2, 2};
  TEST_ASSERT_EQUAL_UINT32(0, buffer.append(more, 4));
  TEST_ASSERT_EQUAL_UINT32(kCapacity, buffer.size());
}

static void reset_empties_the_buffer_for_the_next_turn() {
  tth::TurnBuffer buffer;
  attach(buffer);
  const int16_t chunk[4] = {1, 2, 3, 4};
  buffer.append(chunk, 4);

  buffer.reset();
  TEST_ASSERT_EQUAL_UINT32(0, buffer.size());
  TEST_ASSERT_FALSE(buffer.isFull());
  TEST_ASSERT_EQUAL_UINT32(kCapacity, buffer.remaining());
}

// Two high-water marks: one per turn (what a summary reports) and one for the
// session (what judges the capacity).
static void reset_clears_the_turn_high_water_but_not_the_session_one() {
  tth::TurnBuffer buffer;
  attach(buffer);
  int16_t chunk[40];
  fill(chunk, 40, 3);

  buffer.append(chunk, 40);
  TEST_ASSERT_EQUAL_UINT32(40, buffer.highWaterSamples());

  buffer.reset();
  buffer.append(chunk, 10);
  TEST_ASSERT_EQUAL_UINT32(10, buffer.size());
  // Per-turn: reset with the contents, so it describes THIS turn only.
  TEST_ASSERT_EQUAL_UINT32(10, buffer.highWaterSamples());
  // Session: remembers the longest turn, for judging the capacity.
  TEST_ASSERT_EQUAL_UINT32(40, buffer.sessionHighWaterSamples());
}

// --- PCM processing ---------------------------------------------------------

static void dc_removal_centres_a_constant_offset() {
  int16_t samples[8];
  fill(samples, 8, 1000);

  const tth::ChunkLevels levels = tth::removeDcOffsetAndMeasure(samples, 8);

  for (int i = 0; i < 8; ++i) TEST_ASSERT_EQUAL_INT16(0, samples[i]);
  // A pure DC signal is silence once centred.
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, levels.rms);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, levels.peak);
}

// The offset must be removed without disturbing the signal riding on it.
static void dc_removal_preserves_the_waveform() {
  int16_t samples[4] = {1000 + 100, 1000 - 100, 1000 + 100, 1000 - 100};

  tth::removeDcOffsetAndMeasure(samples, 4);

  TEST_ASSERT_EQUAL_INT16(100, samples[0]);
  TEST_ASSERT_EQUAL_INT16(-100, samples[1]);
  TEST_ASSERT_EQUAL_INT16(100, samples[2]);
  TEST_ASSERT_EQUAL_INT16(-100, samples[3]);
}

// Levels must describe the stored audio, i.e. after the correction.
static void levels_are_measured_after_the_correction() {
  int16_t withOffset[4] = {20000, 20000, 20000, 20000};
  const tth::ChunkLevels corrected =
      tth::removeDcOffsetAndMeasure(withOffset, 4);

  int16_t raw[4] = {20000, 20000, 20000, 20000};
  const tth::ChunkLevels uncorrected = tth::measureLevels(raw, 4);

  TEST_ASSERT_TRUE(uncorrected.rms > 0.5f);
  TEST_ASSERT_TRUE(corrected.rms < 0.01f);
}

// A large offset must not wrap a sample around into the opposite sign.
static void dc_removal_saturates_instead_of_wrapping() {
  int16_t samples[2] = {-32768, 32767};
  tth::removeDcOffsetAndMeasure(samples, 2);
  // Mean is 0 (rounded), so nothing should move; more importantly nothing
  // wrapped.
  TEST_ASSERT_TRUE(samples[0] < 0);
  TEST_ASSERT_TRUE(samples[1] > 0);

  int16_t biased[4] = {-32768, -32768, -32768, 32767};
  tth::removeDcOffsetAndMeasure(biased, 4);
  for (int i = 0; i < 4; ++i) {
    TEST_ASSERT_TRUE(biased[i] >= -32768);
    TEST_ASSERT_TRUE(biased[i] <= 32767);
  }
}

static void levels_are_normalised_and_clamped() {
  int16_t loud[2] = {32767, -32768};
  const tth::ChunkLevels levels = tth::measureLevels(loud, 2);
  TEST_ASSERT_TRUE(levels.rms >= 0.0f && levels.rms <= 1.0f);
  TEST_ASSERT_TRUE(levels.peak >= 0.0f && levels.peak <= 1.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, levels.peak);
}

static void an_empty_chunk_measures_as_silence() {
  const tth::ChunkLevels levels = tth::measureLevels(nullptr, 0);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, levels.rms);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, levels.peak);
}

// --- Phase 5: the two reader invariants --------------------------------------

// Committed-length boundary: the published length only ever covers samples
// that are already fully written.
static void the_committed_length_follows_every_append() {
  tth::TurnBuffer buffer;
  attach(buffer);
  TEST_ASSERT_EQUAL_UINT32(0, buffer.committedSamples());

  int16_t chunk[10];
  fill(chunk, 10, 7);
  buffer.append(chunk, 10);
  TEST_ASSERT_EQUAL_UINT32(10, buffer.committedSamples());
  TEST_ASSERT_EQUAL_UINT32(buffer.size(), buffer.committedSamples());
  for (uint32_t i = 0; i < buffer.committedSamples(); ++i) {
    TEST_ASSERT_EQUAL_INT16(7, buffer.data()[i]);
  }

  // A capacity-clipped append commits only what was actually stored.
  int16_t big[200];
  fill(big, 200, 3);
  buffer.append(big, 200);
  TEST_ASSERT_EQUAL_UINT32(kCapacity, buffer.committedSamples());
}

// Lifetime: while a reader holds a lease, reset() refuses and changes nothing.
static void a_held_lease_blocks_reset_and_changes_nothing() {
  tth::TurnBuffer buffer;
  attach(buffer);
  int16_t chunk[10];
  fill(chunk, 10, 5);
  buffer.append(chunk, 10);

  TEST_ASSERT_TRUE(buffer.retain());
  TEST_ASSERT_FALSE(buffer.reset());
  TEST_ASSERT_EQUAL_UINT32(10, buffer.size());
  TEST_ASSERT_EQUAL_UINT32(10, buffer.committedSamples());
  TEST_ASSERT_EQUAL_UINT32(10, buffer.highWaterSamples());
  TEST_ASSERT_EQUAL_INT16(5, buffer.data()[9]);

  buffer.release();
  TEST_ASSERT_TRUE(buffer.reset());
  TEST_ASSERT_EQUAL_UINT32(0, buffer.committedSamples());
}

static void every_reader_must_let_go_before_a_reset() {
  tth::TurnBuffer buffer;
  attach(buffer);
  TEST_ASSERT_TRUE(buffer.retain());
  TEST_ASSERT_TRUE(buffer.retain());
  TEST_ASSERT_EQUAL_UINT32(2, buffer.readers());

  buffer.release();
  TEST_ASSERT_FALSE(buffer.reset());
  buffer.release();
  TEST_ASSERT_TRUE(buffer.reset());
}

static void leases_never_wrap_and_need_storage() {
  tth::TurnBuffer buffer;
  attach(buffer);
  buffer.release();
  TEST_ASSERT_EQUAL_UINT32(0, buffer.readers());
  TEST_ASSERT_EQUAL_UINT32(1, buffer.unbalancedReleases());
  TEST_ASSERT_TRUE(buffer.reset());

  tth::TurnBuffer unattached;
  TEST_ASSERT_FALSE(unattached.retain());
}

// The streamer reads while the capture is still recording: a lease blocks
// reset, never append.
static void appending_under_a_lease_is_allowed() {
  tth::TurnBuffer buffer;
  attach(buffer);
  TEST_ASSERT_TRUE(buffer.retain());
  int16_t chunk[10];
  fill(chunk, 10, 1);
  TEST_ASSERT_EQUAL_UINT32(10, buffer.append(chunk, 10));
  TEST_ASSERT_EQUAL_UINT32(10, buffer.committedSamples());
  buffer.release();
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(the_committed_length_follows_every_append);
  RUN_TEST(a_held_lease_blocks_reset_and_changes_nothing);
  RUN_TEST(every_reader_must_let_go_before_a_reset);
  RUN_TEST(leases_never_wrap_and_need_storage);
  RUN_TEST(appending_under_a_lease_is_allowed);
  RUN_TEST(an_unattached_buffer_accepts_nothing);
  RUN_TEST(appending_stores_the_samples_in_order);
  RUN_TEST(a_chunk_that_exactly_fills_the_buffer_is_stored_whole);
  RUN_TEST(an_oversized_chunk_is_truncated_at_the_capacity);
  RUN_TEST(nothing_is_ever_written_past_the_end);
  RUN_TEST(a_full_buffer_accepts_nothing_more);
  RUN_TEST(reset_empties_the_buffer_for_the_next_turn);
  RUN_TEST(reset_clears_the_turn_high_water_but_not_the_session_one);
  RUN_TEST(dc_removal_centres_a_constant_offset);
  RUN_TEST(dc_removal_preserves_the_waveform);
  RUN_TEST(levels_are_measured_after_the_correction);
  RUN_TEST(dc_removal_saturates_instead_of_wrapping);
  RUN_TEST(levels_are_normalised_and_clamped);
  RUN_TEST(an_empty_chunk_measures_as_silence);
  return UNITY_END();
}
