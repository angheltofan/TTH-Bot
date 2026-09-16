// Host-side tests for the ordered outbound queue and its send fence.
//
// The central property: on the wire, a turn is always
//
//   turn_start(N) · audio(N)... · turn_end(N)
//
// with turn_end never overtaking audio -- under a slow or stalling sender, a
// partially full queue, Busy retries, and failed sends -- and nothing of turn
// N ever following cancel(N).

#include <string.h>

#include <map>
#include <set>
#include <vector>

#include <unity.h>

#include "tth/GatewayProtocol.h"
#include "tth/OutboundQueue.h"

using tth::OutboundItem;
using tth::OutboundQueue;
using tth::OutKind;
using tth::OutPush;

void setUp() {}
void tearDown() {}

namespace {

struct WireFrame {
  OutKind kind;
  uint32_t turn;
  uint32_t frames;  // turn_end
  uint32_t bytes;   // turn_end / credit
  std::vector<int16_t> pcm;
};

// What went out, decoded back from the actual bytes.
WireFrame decode(const OutboundItem& item) {
  WireFrame f;
  f.kind = item.kind;
  f.turn = item.turn;
  f.frames = 0;
  f.bytes = 0;
  if (item.binary) {
    tth::wire::AudioFrameView view;
    TEST_ASSERT_TRUE(tth::wire::parseAudioFrame(item.bytes, item.length,
                                                tth::wire::kKindUserAudio,
                                                view) ==
                     tth::wire::FrameError::None);
    TEST_ASSERT_EQUAL_UINT32(item.turn, view.turn);
    for (uint32_t i = 0; i + 1 < view.pcmBytes; i += 2) {
      const uint16_t v = static_cast<uint16_t>(view.pcm[i] |
                                               (view.pcm[i + 1] << 8));
      f.pcm.push_back(static_cast<int16_t>(v));
    }
  } else {
    tth::wire::ControlMessage m;
    tth::wire::parseControl(reinterpret_cast<const char*>(item.bytes),
                            item.length, m);
    if (m.hasTurn) TEST_ASSERT_EQUAL_UINT32(item.turn, m.turn);
    f.frames = m.frames;
    f.bytes = m.bytes;
  }
  return f;
}

// Sends one item completely. Returns false when the queue was empty.
bool sendOne(OutboundQueue& q, std::vector<WireFrame>& wire) {
  const OutboundItem* item = q.beginSend();
  if (item == nullptr) return false;
  wire.push_back(decode(*item));
  q.completeSend();
  return true;
}

void drain(OutboundQueue& q, std::vector<WireFrame>& wire) {
  while (sendOne(q, wire)) {
  }
}

// Audio samples that encode (turn, frame index, position), so the receiver
// can prove order, no gaps and no duplicates.
std::vector<int16_t> audioFor(uint32_t turn, uint32_t frame, uint32_t samples) {
  std::vector<int16_t> pcm(samples);
  for (uint32_t i = 0; i < samples; ++i) {
    pcm[i] = static_cast<int16_t>(((turn & 0x3Fu) << 9) ^ (frame << 3) ^ i);
  }
  return pcm;
}

struct Lcg {
  uint32_t state;
  uint32_t next() {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
  }
  uint32_t below(uint32_t n) { return next() % n; }
};

// The property every wire capture must satisfy.
void checkWire(const std::vector<WireFrame>& wire,
               const std::map<uint32_t, uint32_t>& expectedFrames,
               const std::set<uint32_t>& cancelled) {
  std::map<uint32_t, int> state;  // 0 none, 1 started, 2 ended
  std::map<uint32_t, uint32_t> frames;
  std::map<uint32_t, uint32_t> bytes;
  std::set<uint32_t> cancelSeen;

  for (size_t i = 0; i < wire.size(); ++i) {
    const WireFrame& f = wire[i];
    if (f.kind == OutKind::Credit || f.kind == OutKind::Ping) continue;
    const uint32_t turn = f.turn;

    if (cancelSeen.count(turn) != 0) {
      TEST_FAIL_MESSAGE("an item of turn N followed cancel(N)");
    }
    switch (f.kind) {
      case OutKind::TurnStart:
        if (state[turn] != 0) TEST_FAIL_MESSAGE("turn_start twice");
        state[turn] = 1;
        break;
      case OutKind::Audio: {
        if (state[turn] != 1) {
          TEST_FAIL_MESSAGE("audio outside turn_start..turn_end");
        }
        const std::vector<int16_t> want =
            audioFor(turn, frames[turn], static_cast<uint32_t>(f.pcm.size()));
        if (want != f.pcm) TEST_FAIL_MESSAGE("audio out of order or damaged");
        ++frames[turn];
        bytes[turn] += static_cast<uint32_t>(f.pcm.size() * 2);
        break;
      }
      case OutKind::TurnEnd:
        if (state[turn] != 1) TEST_FAIL_MESSAGE("turn_end without turn_start");
        // THE FENCE: every audio frame of the turn is already on the wire.
        TEST_ASSERT_EQUAL_UINT32(frames[turn], f.frames);
        TEST_ASSERT_EQUAL_UINT32(bytes[turn], f.bytes);
        state[turn] = 2;
        break;
      case OutKind::Cancel:
        if (state[turn] == 0) TEST_FAIL_MESSAGE("cancel for a turn never started");
        cancelSeen.insert(turn);
        break;
      case OutKind::Credit:
      case OutKind::Ping:
        break;
    }
  }

  for (const auto& e : expectedFrames) {
    const uint32_t turn = e.first;
    if (cancelled.count(turn) != 0) continue;
    if (state[turn] != 2) TEST_FAIL_MESSAGE("a completed turn is missing turn_end");
    TEST_ASSERT_EQUAL_UINT32(e.second, frames[turn]);
  }
}

}  // namespace

// --- basics ---------------------------------------------------------------------------

static void a_turn_goes_out_in_order() {
  OutboundQueue q;
  TEST_ASSERT_TRUE(q.pushTurnStart(5) == OutPush::Accepted);
  for (uint32_t i = 0; i < 5; ++i) {
    const std::vector<int16_t> pcm = audioFor(5, i, 320);
    TEST_ASSERT_TRUE(q.pushAudio(5, pcm.data(), 320) == OutPush::Accepted);
  }
  TEST_ASSERT_TRUE(q.pushTurnEnd(5, 5, 3200) == OutPush::Accepted);

  std::vector<WireFrame> wire;
  drain(q, wire);
  TEST_ASSERT_EQUAL_UINT32(7, wire.size());
  TEST_ASSERT_TRUE(wire[0].kind == OutKind::TurnStart);
  for (int i = 1; i <= 5; ++i) TEST_ASSERT_TRUE(wire[i].kind == OutKind::Audio);
  TEST_ASSERT_TRUE(wire[6].kind == OutKind::TurnEnd);
  checkWire(wire, {{5, 5}}, {});
}

static void audio_is_written_little_endian() {
  OutboundQueue q;
  q.pushTurnStart(1);
  const int16_t pcm[2] = {0x1234, static_cast<int16_t>(0xFEDC)};
  q.pushAudio(1, pcm, 2);
  q.beginSend();
  q.completeSend();  // turn_start
  const OutboundItem* item = q.beginSend();
  TEST_ASSERT_TRUE(item->binary);
  TEST_ASSERT_EQUAL_UINT32(8 + 4, item->length);
  TEST_ASSERT_EQUAL_HEX8(0x34, item->bytes[8]);
  TEST_ASSERT_EQUAL_HEX8(0x12, item->bytes[9]);
  TEST_ASSERT_EQUAL_HEX8(0xDC, item->bytes[10]);
  TEST_ASSERT_EQUAL_HEX8(0xFE, item->bytes[11]);
}

// An item leaves the queue only after completeSend(): until then the sender
// keeps getting the same one.
static void an_item_is_removed_only_after_a_successful_send() {
  OutboundQueue q;
  q.pushTurnStart(1);
  q.pushTurnEnd(1, 0, 0);
  const OutboundItem* first = q.beginSend();
  TEST_ASSERT_TRUE(first->kind == OutKind::TurnStart);
  TEST_ASSERT_EQUAL_PTR(first, q.beginSend());
  TEST_ASSERT_EQUAL_UINT32(2, q.size());
  q.completeSend();
  TEST_ASSERT_EQUAL_UINT32(1, q.size());
  TEST_ASSERT_TRUE(q.beginSend()->kind == OutKind::TurnEnd);
}

static void a_failed_send_clears_everything() {
  OutboundQueue q;
  q.pushTurnStart(1);
  const std::vector<int16_t> pcm = audioFor(1, 0, 320);
  q.pushAudio(1, pcm.data(), 320);
  q.beginSend();
  q.failSend();
  TEST_ASSERT_EQUAL_UINT32(0, q.size());
  TEST_ASSERT_EQUAL_UINT32(0, q.openTurn());
  TEST_ASSERT_EQUAL_UINT32(OutboundQueue::kSlots, q.freeSlots());
  TEST_ASSERT_NULL(q.beginSend());
  // A fresh turn works on the new connection.
  TEST_ASSERT_TRUE(q.pushTurnStart(2) == OutPush::Accepted);
}

// Priority items wait for the frame boundary, then go before the rest.
static void priority_items_go_out_only_at_frame_boundaries() {
  OutboundQueue q;
  q.pushTurnStart(1);
  const std::vector<int16_t> a = audioFor(1, 0, 320);
  const std::vector<int16_t> b = audioFor(1, 1, 320);
  q.pushAudio(1, a.data(), 320);
  q.pushAudio(1, b.data(), 320);

  q.beginSend();
  q.completeSend();                              // turn_start
  const OutboundItem* inFlight = q.beginSend();  // audio #0, mid-send
  TEST_ASSERT_TRUE(inFlight->kind == OutKind::Audio);
  q.pushCredit(3840);
  TEST_ASSERT_EQUAL_PTR(inFlight, q.beginSend());  // not interrupted
  q.completeSend();
  TEST_ASSERT_TRUE(q.beginSend()->kind == OutKind::Credit);
  q.completeSend();
  TEST_ASSERT_TRUE(q.beginSend()->kind == OutKind::Audio);
}

// Audio cannot use the reserved slots, so a turn can always be closed.
static void reserved_slots_keep_turn_end_possible() {
  OutboundQueue q;
  q.pushTurnStart(1);
  const std::vector<int16_t> pcm = audioFor(1, 0, 320);
  uint32_t accepted = 0;
  while (q.pushAudio(1, pcm.data(), 320) == OutPush::Accepted) ++accepted;
  TEST_ASSERT_EQUAL_UINT32(OutboundQueue::kSlots - OutboundQueue::kReservedSlots - 1,
                           accepted);
  TEST_ASSERT_TRUE(q.pushAudio(1, pcm.data(), 320) == OutPush::Busy);
  TEST_ASSERT_TRUE(q.pushTurnEnd(1, accepted, accepted * 640) == OutPush::Accepted);
  TEST_ASSERT_TRUE(q.pushCredit(100) == OutPush::Accepted);
}

static void invalid_pushes_are_refused() {
  OutboundQueue q;
  const std::vector<int16_t> pcm = audioFor(1, 0, 321);
  TEST_ASSERT_TRUE(q.pushAudio(1, pcm.data(), 10) == OutPush::Invalid);  // no turn
  TEST_ASSERT_TRUE(q.pushTurnStart(0) == OutPush::Invalid);
  TEST_ASSERT_TRUE(q.pushTurnStart(1) == OutPush::Accepted);
  TEST_ASSERT_TRUE(q.pushTurnStart(2) == OutPush::Invalid);  // 1 still open
  TEST_ASSERT_TRUE(q.pushAudio(2, pcm.data(), 10) == OutPush::Invalid);
  TEST_ASSERT_TRUE(q.pushAudio(1, pcm.data(), 321) == OutPush::Invalid);
  TEST_ASSERT_TRUE(q.pushAudio(1, pcm.data(), 0) == OutPush::Invalid);
  TEST_ASSERT_TRUE(q.pushTurnEnd(2, 0, 0) == OutPush::Invalid);
}

static void credit_messages_coalesce() {
  OutboundQueue q;
  q.pushCredit(100);
  q.pushCredit(200);
  TEST_ASSERT_EQUAL_UINT32(1, q.size());
  std::vector<WireFrame> wire;
  drain(q, wire);
  TEST_ASSERT_EQUAL_UINT32(300, wire[0].bytes);
}

// --- the fence under stress -----------------------------------------------------------

// A slow, stalling sender and a producer that keeps the queue partly full and
// retries on Busy. Whatever the interleaving, turn_end never overtakes audio,
// and every frame arrives once, in order.
static void turn_end_never_overtakes_audio_under_a_delayed_sender() {
  OutboundQueue q;
  Lcg rng = {12345};
  std::vector<WireFrame> wire;
  std::map<uint32_t, uint32_t> expected;

  for (uint32_t turn = 1; turn <= 300; ++turn) {
    const uint32_t audioFrames = 1 + rng.below(60);
    expected[turn] = audioFrames;
    uint32_t pushed = 0;
    bool started = false;
    bool ended = false;
    uint32_t bytes = 0;
    while (!ended) {
      // Producer: one step per tick, retrying on Busy.
      if (!started) {
        if (q.pushTurnStart(turn) == OutPush::Accepted) started = true;
      } else if (pushed < audioFrames) {
        const uint32_t samples = (pushed + 1 == audioFrames) ? 1 + rng.below(320)
                                                             : 320;
        const std::vector<int16_t> pcm = audioFor(turn, pushed, samples);
        if (q.pushAudio(turn, pcm.data(), samples) == OutPush::Accepted) {
          ++pushed;
          bytes += samples * 2;
        }
      } else if (q.pushTurnEnd(turn, pushed, bytes) == OutPush::Accepted) {
        ended = true;
      }
      if (rng.below(7) == 0) q.pushCredit(1 + rng.below(4000));

      // Sender: sometimes stalls, sometimes sends a burst, sometimes leaves a
      // frame in flight across ticks.
      const uint32_t burst = rng.below(4);  // 0 = stalled this tick
      for (uint32_t i = 0; i < burst; ++i) {
        const OutboundItem* item = q.beginSend();
        if (item == nullptr) break;
        if (rng.below(5) == 0) break;  // still in flight; completes later
        wire.push_back(decode(*item));
        q.completeSend();
      }
    }
  }
  // Finish whatever is left, including an item left in flight.
  while (true) {
    const OutboundItem* item = q.beginSend();
    if (item == nullptr) break;
    wire.push_back(decode(*item));
    q.completeSend();
  }
  checkWire(wire, expected, {});
  TEST_ASSERT_TRUE(q.counters().busy > 0);  // the queue really was full at times
}

// --- cancel ------------------------------------------------------------------------------

// Barge-in: turn N is on the wire, its tail is still queued. cancel(N) purges
// the unsent tail and goes out before turn_start(N+1).
static void cancel_purges_the_unsent_tail_and_precedes_the_next_turn() {
  OutboundQueue q;
  q.pushTurnStart(7);
  for (uint32_t i = 0; i < 3; ++i) {
    const std::vector<int16_t> pcm = audioFor(7, i, 320);
    q.pushAudio(7, pcm.data(), 320);
  }
  q.pushTurnEnd(7, 3, 1920);
  std::vector<WireFrame> wire;
  sendOne(q, wire);  // turn_start(7) reached the wire

  q.requestCancel(7);
  q.pushTurnStart(8);
  const std::vector<int16_t> pcm = audioFor(8, 0, 320);
  q.pushAudio(8, pcm.data(), 320);
  q.pushTurnEnd(8, 1, 640);
  drain(q, wire);

  TEST_ASSERT_EQUAL_UINT32(5, wire.size());
  TEST_ASSERT_TRUE(wire[0].kind == OutKind::TurnStart && wire[0].turn == 7);
  TEST_ASSERT_TRUE(wire[1].kind == OutKind::Cancel && wire[1].turn == 7);
  TEST_ASSERT_TRUE(wire[2].kind == OutKind::TurnStart && wire[2].turn == 8);
  TEST_ASSERT_EQUAL_UINT32(4, q.counters().purged);  // 3 audio + turn_end
  TEST_ASSERT_EQUAL_UINT32(1, q.counters().cancelsSent);
  checkWire(wire, {{8, 1}}, {7});
}

// The gateway never heard of the turn: nothing of it, and no cancel, is sent.
static void cancel_of_a_turn_never_on_the_wire_is_dropped() {
  OutboundQueue q;
  q.pushTurnStart(3);
  const std::vector<int16_t> pcm = audioFor(3, 0, 320);
  q.pushAudio(3, pcm.data(), 320);
  q.requestCancel(3);
  TEST_ASSERT_EQUAL_UINT32(0, q.openTurn());

  std::vector<WireFrame> wire;
  drain(q, wire);
  TEST_ASSERT_EQUAL_UINT32(0, wire.size());
  TEST_ASSERT_EQUAL_UINT32(1, q.counters().cancelsDropped);
}

// A cancel never cuts a frame in half: the in-flight audio frame completes,
// and the cancel follows at the boundary.
static void cancel_waits_for_the_frame_in_flight() {
  OutboundQueue q;
  q.pushTurnStart(9);
  for (uint32_t i = 0; i < 3; ++i) {
    const std::vector<int16_t> pcm = audioFor(9, i, 320);
    q.pushAudio(9, pcm.data(), 320);
  }
  std::vector<WireFrame> wire;
  sendOne(q, wire);                               // turn_start(9)
  const OutboundItem* inFlight = q.beginSend();   // audio #0
  q.requestCancel(9);
  TEST_ASSERT_EQUAL_PTR(inFlight, q.beginSend());
  wire.push_back(decode(*inFlight));
  q.completeSend();
  drain(q, wire);

  TEST_ASSERT_EQUAL_UINT32(3, wire.size());
  TEST_ASSERT_TRUE(wire[1].kind == OutKind::Audio);
  TEST_ASSERT_TRUE(wire[2].kind == OutKind::Cancel);
}

// Random barge-ins across hundreds of turns: nothing of a turn ever follows
// its cancel, and every uncancelled turn still arrives whole and in order.
static void nothing_of_a_turn_ever_follows_its_cancel() {
  OutboundQueue q;
  Lcg rng = {777};
  std::vector<WireFrame> wire;
  std::map<uint32_t, uint32_t> expected;
  std::set<uint32_t> cancelled;

  for (uint32_t turn = 1; turn <= 300; ++turn) {
    const uint32_t audioFrames = 1 + rng.below(30);
    const bool cancel = rng.below(3) == 0;
    const uint32_t cancelAt = rng.below(audioFrames + 2);
    expected[turn] = audioFrames;
    uint32_t pushed = 0;
    uint32_t bytes = 0;
    bool started = false;
    bool done = false;
    uint32_t step = 0;
    while (!done) {
      if (cancel && step == cancelAt && started) {
        q.requestCancel(turn);
        cancelled.insert(turn);
        done = true;
      } else if (!started) {
        if (q.pushTurnStart(turn) == OutPush::Accepted) started = true;
      } else if (pushed < audioFrames) {
        const std::vector<int16_t> pcm = audioFor(turn, pushed, 320);
        if (q.pushAudio(turn, pcm.data(), 320) == OutPush::Accepted) {
          ++pushed;
          bytes += 640;
          ++step;
        }
      } else if (q.pushTurnEnd(turn, pushed, bytes) == OutPush::Accepted) {
        done = true;
        // A cancel may also arrive after turn_end (barge-in during speech).
        if (cancel && cancelAt > audioFrames) {
          q.requestCancel(turn);
          cancelled.insert(turn);
        }
      }
      const uint32_t burst = rng.below(3);
      for (uint32_t i = 0; i < burst; ++i) {
        if (!sendOne(q, wire)) break;
      }
    }
  }
  drain(q, wire);
  checkWire(wire, expected, cancelled);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(a_turn_goes_out_in_order);
  RUN_TEST(audio_is_written_little_endian);
  RUN_TEST(an_item_is_removed_only_after_a_successful_send);
  RUN_TEST(a_failed_send_clears_everything);
  RUN_TEST(priority_items_go_out_only_at_frame_boundaries);
  RUN_TEST(reserved_slots_keep_turn_end_possible);
  RUN_TEST(invalid_pushes_are_refused);
  RUN_TEST(credit_messages_coalesce);
  RUN_TEST(turn_end_never_overtakes_audio_under_a_delayed_sender);
  RUN_TEST(cancel_purges_the_unsent_tail_and_precedes_the_next_turn);
  RUN_TEST(cancel_of_a_turn_never_on_the_wire_is_dropped);
  RUN_TEST(cancel_waits_for_the_frame_in_flight);
  RUN_TEST(nothing_of_a_turn_ever_follows_its_cancel);
  return UNITY_END();
}
