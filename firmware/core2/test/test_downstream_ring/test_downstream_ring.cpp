// Host-side tests for the downstream ring and the Step 6.3 credit additions:
// publication (no partial frame is ever visible), wrap-around, refusal without
// damage, connection tags, a deterministic producer/consumer interleaving, and
// credit conservation against the ring's real contents.

#include <string.h>

#include <vector>

#include <unity.h>

#include "tth/DownstreamCredit.h"
#include "tth/DownstreamRing.h"

using tth::DownstreamCredit;
using tth::DownstreamRing;
using tth::RingFrame;
using tth::RingWrite;

void setUp() {}
void tearDown() {}

namespace {

std::vector<uint8_t> pattern(uint32_t bytes, uint8_t seed) {
  std::vector<uint8_t> out(bytes);
  for (uint32_t i = 0; i < bytes; ++i) out[i] = static_cast<uint8_t>(seed + i * 7u);
  return out;
}

// Pops the front frame and returns its payload.
std::vector<uint8_t> take(DownstreamRing& ring, RingFrame& frame) {
  TEST_ASSERT_TRUE(ring.front(frame));
  std::vector<uint8_t> out(frame.pcmBytes);
  TEST_ASSERT_EQUAL_UINT32(frame.pcmBytes, ring.copyPcm(0, out.data(), frame.pcmBytes));
  ring.popFront();
  return out;
}

// Deterministic generator for the interleaving test.
struct Lcg {
  uint32_t state;
  explicit Lcg(uint32_t seed) : state(seed) {}
  uint32_t next() {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
  }
};

}  // namespace

static void an_empty_ring_has_nothing_to_read() {
  std::vector<uint8_t> storage(256);
  DownstreamRing ring;
  TEST_ASSERT_TRUE(ring.attach(storage.data(), 256));
  RingFrame frame;
  TEST_ASSERT_FALSE(ring.front(frame));
  TEST_ASSERT_EQUAL_UINT32(0, ring.usedBytes());
  TEST_ASSERT_EQUAL_UINT32(256, ring.freeBytes());
  TEST_ASSERT_FALSE(DownstreamRing().attach(nullptr, 256));
}

static void a_frame_round_trips_with_its_tags() {
  std::vector<uint8_t> storage(256);
  DownstreamRing ring;
  ring.attach(storage.data(), 256);
  const std::vector<uint8_t> pcm = pattern(40, 3);
  TEST_ASSERT_TRUE(ring.write(7, 42, pcm.data(), 40) == RingWrite::Written);
  TEST_ASSERT_EQUAL_UINT32(DownstreamRing::kHeaderBytes + 40, ring.usedBytes());
  RingFrame frame;
  const std::vector<uint8_t> got = take(ring, frame);
  TEST_ASSERT_EQUAL_UINT32(7, frame.connection);
  TEST_ASSERT_EQUAL_UINT32(42, frame.turn);
  TEST_ASSERT_EQUAL_UINT32(40, frame.pcmBytes);
  TEST_ASSERT_EQUAL_MEMORY(pcm.data(), got.data(), 40);
  TEST_ASSERT_EQUAL_UINT32(0, ring.usedBytes());
}

static void partial_reads_follow_the_offset() {
  std::vector<uint8_t> storage(256);
  DownstreamRing ring;
  ring.attach(storage.data(), 256);
  const std::vector<uint8_t> pcm = pattern(100, 9);
  ring.write(1, 1, pcm.data(), 100);
  uint8_t part[64];
  TEST_ASSERT_EQUAL_UINT32(60, ring.copyPcm(0, part, 60));
  TEST_ASSERT_EQUAL_MEMORY(pcm.data(), part, 60);
  TEST_ASSERT_EQUAL_UINT32(40, ring.copyPcm(60, part, 64));
  TEST_ASSERT_EQUAL_MEMORY(pcm.data() + 60, part, 40);
  TEST_ASSERT_EQUAL_UINT32(0, ring.copyPcm(100, part, 64));
}

// The consumer can never observe a frame before its whole record is written.
static void a_staged_frame_is_invisible_until_committed() {
  std::vector<uint8_t> storage(256);
  DownstreamRing ring;
  ring.attach(storage.data(), 256);
  const std::vector<uint8_t> pcm = pattern(50, 1);
  TEST_ASSERT_TRUE(ring.stage(2, 5, pcm.data(), 50) == RingWrite::Written);
  RingFrame frame;
  TEST_ASSERT_FALSE(ring.front(frame));
  TEST_ASSERT_EQUAL_UINT32(0, ring.usedBytes());
  // A second stage before commit is refused, so a record cannot interleave.
  TEST_ASSERT_TRUE(ring.stage(2, 5, pcm.data(), 50) == RingWrite::Invalid);
  ring.commit();
  TEST_ASSERT_TRUE(ring.front(frame));
  TEST_ASSERT_EQUAL_UINT32(50, frame.pcmBytes);
}

static void records_wrap_around_the_storage_intact() {
  std::vector<uint8_t> storage(64);
  DownstreamRing ring;
  ring.attach(storage.data(), 64);
  for (uint32_t i = 0; i < 200; ++i) {
    const uint32_t bytes = 2 + (i % 5) * 6;  // 2..26
    const std::vector<uint8_t> pcm = pattern(bytes, static_cast<uint8_t>(i));
    TEST_ASSERT_TRUE(ring.write(1, i + 1, pcm.data(), bytes) == RingWrite::Written);
    RingFrame frame;
    const std::vector<uint8_t> got = take(ring, frame);
    TEST_ASSERT_EQUAL_UINT32(i + 1, frame.turn);
    TEST_ASSERT_EQUAL_MEMORY(pcm.data(), got.data(), bytes);
  }
  TEST_ASSERT_EQUAL_UINT32(0, ring.corrupt());
}

// A record that does not fit is refused whole; nothing already there changes.
static void no_space_refuses_the_whole_frame_and_keeps_earlier_data() {
  std::vector<uint8_t> storage(100);
  DownstreamRing ring;
  ring.attach(storage.data(), 100);
  const std::vector<uint8_t> first = pattern(60, 4);
  TEST_ASSERT_TRUE(ring.write(1, 1, first.data(), 60) == RingWrite::Written);  // 72 used
  const std::vector<uint8_t> second = pattern(20, 8);
  TEST_ASSERT_TRUE(ring.write(1, 1, second.data(), 20) == RingWrite::NoSpace);  // needs 32
  TEST_ASSERT_EQUAL_UINT32(72, ring.usedBytes());
  RingFrame frame;
  const std::vector<uint8_t> got = take(ring, frame);
  TEST_ASSERT_EQUAL_MEMORY(first.data(), got.data(), 60);
  TEST_ASSERT_FALSE(ring.front(frame));
  TEST_ASSERT_TRUE(ring.write(1, 1, second.data(), 20) == RingWrite::Written);
}

static void invalid_writes_are_refused() {
  std::vector<uint8_t> storage(64);
  DownstreamRing ring;
  const uint8_t pcm[2] = {1, 2};
  TEST_ASSERT_TRUE(ring.write(1, 1, pcm, 2) == RingWrite::Invalid);  // not attached
  ring.attach(storage.data(), 64);
  TEST_ASSERT_TRUE(ring.write(1, 1, nullptr, 2) == RingWrite::Invalid);
  TEST_ASSERT_TRUE(ring.write(1, 1, pcm, 0) == RingWrite::Invalid);
}

static void the_accepted_connection_and_high_water_are_published() {
  std::vector<uint8_t> storage(256);
  DownstreamRing ring;
  ring.attach(storage.data(), 256);
  ring.acceptConnection(9);
  TEST_ASSERT_EQUAL_UINT32(9, ring.acceptedConnection());
  const std::vector<uint8_t> pcm = pattern(30, 2);
  ring.write(9, 1, pcm.data(), 30);
  ring.write(9, 1, pcm.data(), 30);
  RingFrame frame;
  take(ring, frame);
  TEST_ASSERT_EQUAL_UINT32(2 * (DownstreamRing::kHeaderBytes + 30), ring.highWaterBytes());
}

// A deterministic interleaving of producer and consumer steps, including the
// consumer looking at the ring while a record is staged but not committed.
static void interleaved_producer_and_consumer_never_lose_reorder_or_see_partials() {
  std::vector<uint8_t> storage(300);
  DownstreamRing ring;
  ring.attach(storage.data(), 300);
  Lcg rng(12345);
  uint32_t produced = 0;
  uint32_t consumed = 0;
  bool staged = false;
  std::vector<uint8_t> stagedPcm;
  for (int step = 0; step < 50000; ++step) {
    const uint32_t action = rng.next() % 4;
    if (action == 0 && !staged) {
      const uint32_t bytes = 2 + (rng.next() % 40) * 2;
      stagedPcm = pattern(bytes, static_cast<uint8_t>(produced));
      if (ring.stage(1, produced + 1, stagedPcm.data(), bytes) == RingWrite::Written) {
        staged = true;
      }
    } else if (action == 1 && staged) {
      ring.commit();
      staged = false;
      ++produced;
    } else {
      RingFrame frame;
      if (ring.front(frame)) {
        // Only committed records are ever visible, strictly in order.
        TEST_ASSERT_TRUE(consumed < produced);
        TEST_ASSERT_EQUAL_UINT32(consumed + 1, frame.turn);
        std::vector<uint8_t> got(frame.pcmBytes);
        ring.copyPcm(0, got.data(), frame.pcmBytes);
        const std::vector<uint8_t> expected =
            pattern(frame.pcmBytes, static_cast<uint8_t>(consumed));
        TEST_ASSERT_EQUAL_MEMORY(expected.data(), got.data(), frame.pcmBytes);
        ring.popFront();
        ++consumed;
      }
    }
  }
  TEST_ASSERT_TRUE(produced > 1000);
  TEST_ASSERT_EQUAL_UINT32(0, ring.corrupt());
}

// --- credit -----------------------------------------------------------------------

static void a_return_is_committed_only_after_it_was_queued() {
  DownstreamCredit credit(192000, 3840);
  TEST_ASSERT_TRUE(credit.admit(1920));
  TEST_ASSERT_TRUE(credit.admit(1920));
  credit.consumed(1920);
  TEST_ASSERT_EQUAL_UINT32(0, credit.pendingReturn(false));  // below the batch
  TEST_ASSERT_EQUAL_UINT32(1920, credit.pendingReturn(true));
  credit.consumed(1920);
  // 3840 B = two 1920 B (40 ms) frames = one batch.
  TEST_ASSERT_EQUAL_UINT32(3840, credit.pendingReturn(false));
  // Not queued (outbound Busy): nothing is marked returned.
  TEST_ASSERT_EQUAL_UINT32(3840, credit.pendingReturn(false));
  TEST_ASSERT_EQUAL_UINT32(0, credit.returned());
  credit.commitReturn(3840);
  TEST_ASSERT_EQUAL_UINT32(3840, credit.returned());
  TEST_ASSERT_EQUAL_UINT32(0, credit.pendingReturn(true));
  credit.commitReturn(100);  // more than pending: clamped
  TEST_ASSERT_EQUAL_UINT32(3840, credit.returned());
}

static void per_connection_epochs_start_each_side_from_zero() {
  DownstreamCredit credit(10000, 1000);
  credit.admit(4000);
  credit.consumed(4000);
  credit.commitReturn(3000);
  credit.beginConsumerEpoch();
  credit.beginProducerEpoch();
  TEST_ASSERT_EQUAL_UINT32(0, credit.received());
  TEST_ASSERT_EQUAL_UINT32(0, credit.returned());
  TEST_ASSERT_EQUAL_UINT32(0, credit.consumedBytes());
  TEST_ASSERT_EQUAL_UINT32(10000, credit.gatewayRemaining());
  TEST_ASSERT_TRUE(credit.admit(10000));
  TEST_ASSERT_EQUAL_UINT32(0, credit.gatewayRemaining());
  TEST_ASSERT_FALSE(credit.admit(2));
  TEST_ASSERT_EQUAL_UINT32(1, credit.violations());
}

// Every granted byte is in exactly one place: still with the gateway, in the
// ring, or consumed and awaiting return -- checked against the ring's real
// contents, through admits, partial consumption, stale drops and returns.
static void credit_is_conserved_against_the_ring_contents() {
  const uint32_t kCapacity = 19200;
  std::vector<uint8_t> storage(kCapacity + 64 * DownstreamRing::kHeaderBytes);
  DownstreamRing ring;
  ring.attach(storage.data(), static_cast<uint32_t>(storage.size()));
  DownstreamCredit credit(kCapacity, 3840);
  Lcg rng(777);
  uint32_t ringPcm = 0;  // PCM bytes actually published and not yet consumed
  uint32_t frontOffset = 0;
  for (int step = 0; step < 20000; ++step) {
    const uint32_t action = rng.next() % 3;
    if (action == 0) {
      const uint32_t bytes = 2 + (rng.next() % 960) * 2;
      const std::vector<uint8_t> pcm = pattern(bytes, 1);
      // A well-behaved gateway sends only within the credit it still holds.
      if (bytes <= credit.gatewayRemaining() &&
          ring.freeBytes() >= DownstreamRing::kHeaderBytes + bytes) {
        TEST_ASSERT_TRUE(credit.admit(bytes));
        TEST_ASSERT_TRUE(ring.write(1, 1, pcm.data(), bytes) == RingWrite::Written);
        ringPcm += bytes;
      }
    } else if (action == 1) {
      RingFrame frame;
      if (ring.front(frame)) {
        uint32_t take = 2 + (rng.next() % 1000) * 2;
        const uint32_t left = frame.pcmBytes - frontOffset;
        if (take > left) take = left;
        credit.consumed(take);
        ringPcm -= take;
        frontOffset += take;
        if (frontOffset == frame.pcmBytes) {
          ring.popFront();
          frontOffset = 0;
        }
      }
    } else {
      const uint32_t pending = credit.pendingReturn((rng.next() & 1u) != 0);
      if (pending > 0 && (rng.next() % 4) != 0) credit.commitReturn(pending);
    }
    const uint32_t remaining = credit.gatewayRemaining();
    const uint32_t awaiting = credit.consumedBytes() - credit.returned();
    TEST_ASSERT_EQUAL_UINT32(ringPcm, credit.inRing());
    TEST_ASSERT_EQUAL_UINT32(kCapacity, remaining + credit.inRing() + awaiting);
  }
  TEST_ASSERT_EQUAL_UINT32(0, credit.violations());
  TEST_ASSERT_EQUAL_UINT32(0, credit.overConsumed());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(an_empty_ring_has_nothing_to_read);
  RUN_TEST(a_frame_round_trips_with_its_tags);
  RUN_TEST(partial_reads_follow_the_offset);
  RUN_TEST(a_staged_frame_is_invisible_until_committed);
  RUN_TEST(records_wrap_around_the_storage_intact);
  RUN_TEST(no_space_refuses_the_whole_frame_and_keeps_earlier_data);
  RUN_TEST(invalid_writes_are_refused);
  RUN_TEST(the_accepted_connection_and_high_water_are_published);
  RUN_TEST(interleaved_producer_and_consumer_never_lose_reorder_or_see_partials);
  RUN_TEST(a_return_is_committed_only_after_it_was_queued);
  RUN_TEST(per_connection_epochs_start_each_side_from_zero);
  RUN_TEST(credit_is_conserved_against_the_ring_contents);
  return UNITY_END();
}
