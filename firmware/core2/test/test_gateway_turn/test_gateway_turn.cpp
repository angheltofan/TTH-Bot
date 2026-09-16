// Host-side tests for GatewayTurnSource (Step 6.3): outbound ordering and
// backpressure, the first-response timeout, cancel / barge-in purges, session
// loss in every phase, protocol checks, credit conservation over a response
// longer than the ring, the TurnBuffer lease under backpressure, and a
// deterministic interleaving of the loop and the sender.

#include <string.h>

#include <map>
#include <vector>

#include <unity.h>

#include "tth/DownstreamCredit.h"
#include "tth/DownstreamRing.h"
#include "tth/GatewayTurnSource.h"
#include "tth/TurnBuffer.h"
#include "tth/TurnStreamer.h"

using namespace tth;

void setUp() {}
void tearDown() {}

namespace {

const uint32_t kCredit = 192000;
const uint32_t kRingBytes = kCredit + 8192u * DownstreamRing::kHeaderBytes;
const uint32_t kFrame = 1920;

struct Sent {
  OutKind kind;
  uint32_t turn;
  uint32_t value;
};

// The OutboundQueue plus a NetSender that writes whole items.
class TestUplink : public IUplink {
 public:
  OutboundQueue q;
  uint32_t lastEnd = 0;
  bool busyAudio = false;
  bool busyTurnEnd = false;
  bool busyCredit = false;
  std::vector<Sent> wire;

  OutPush pushTurnStart(uint32_t turn) override { return q.pushTurnStart(turn); }
  OutPush pushAudio(uint32_t turn, const int16_t* pcm, uint32_t samples) override {
    return busyAudio ? OutPush::Busy : q.pushAudio(turn, pcm, samples);
  }
  OutPush pushTurnEnd(uint32_t turn, uint32_t frames, uint32_t bytes) override {
    return busyTurnEnd ? OutPush::Busy : q.pushTurnEnd(turn, frames, bytes);
  }
  void requestCancel(uint32_t turn) override { q.requestCancel(turn); }
  OutPush pushCredit(uint32_t bytes) override {
    return busyCredit ? OutPush::Busy : q.pushCredit(bytes);
  }
  uint32_t lastTurnEndSent() const override { return lastEnd; }

  uint32_t send(uint32_t max = 100000) {
    uint32_t n = 0;
    while (n < max) {
      const OutboundItem* item = q.beginSend();
      if (item == nullptr) break;
      const Sent s = {item->kind, item->turn, item->value};
      q.completeSend();
      wire.push_back(s);
      if (s.kind == OutKind::TurnEnd) lastEnd = s.turn;
      ++n;
    }
    return n;
  }

  uint32_t creditDelivered() const {
    uint32_t sum = 0;
    for (const Sent& s : wire) {
      if (s.kind == OutKind::Credit) sum += s.value;
    }
    return sum;
  }

  std::vector<OutKind> kindsFor(uint32_t turn) const {
    std::vector<OutKind> out;
    for (const Sent& s : wire) {
      if (s.turn == turn && s.kind != OutKind::Credit) out.push_back(s.kind);
    }
    return out;
  }
};

GatewayTurnConfig makeConfig() {
  GatewayTurnConfig c;
  c.captureFormat = monoS16(16000);
  c.responseFormat = monoS16(24000);
  c.firstResponseTimeoutMs = 10000;
  c.maxUpSamples = 320;
  return c;
}

wire::ControlMessage control(wire::ControlType type, uint32_t turn) {
  wire::ControlMessage m;
  memset(&m, 0, sizeof(m));
  m.type = type;
  m.turn = turn;
  m.hasTurn = true;
  return m;
}

wire::ControlMessage speechStart(uint32_t turn, const char* format = "s16le/24000/1") {
  wire::ControlMessage m = control(wire::ControlType::SpeechStart, turn);
  strncpy(m.format, format, sizeof(m.format) - 1);
  return m;
}

wire::ControlMessage turnComplete(uint32_t turn, uint32_t frames, uint32_t bytes) {
  wire::ControlMessage m = control(wire::ControlType::TurnComplete, turn);
  m.frames = frames;
  m.bytes = bytes;
  return m;
}

enum class Produced { Written, WrongConnection, OverCapacity, OverCredit };

struct Rig {
  TestUplink up;
  std::vector<uint8_t> storage;
  DownstreamRing ring;
  DownstreamCredit credit;
  GatewayTurnSource source;
  int16_t pcm[320];
  uint32_t now = 1000;
  // The gateway's side of the credit, per connection.
  uint32_t gwSent = 0;
  uint32_t gwReturnedBase = 0;
  uint16_t nextSample = 0;
  uint16_t expectedSample = 0;
  uint32_t violations = 0;

  Rig()
      : storage(kRingBytes),
        credit(kCredit, 3840),
        source(up, ring, credit, makeConfig()) {
    ring.attach(storage.data(), kRingBytes);
    for (int i = 0; i < 320; ++i) pcm[i] = static_cast<int16_t>(i);
  }

  // What App and GatewayClient do for a new connection, in their order.
  void connect(uint32_t connection) {
    source.onConnecting(connection, now);  // loop, before the connect command
    up.q.reset();                          // network task, at doConnect
    credit.beginProducerEpoch();
    gwSent = 0;
    gwReturnedBase = up.creditDelivered();
    source.onReady(now);                   // session READY
  }

  uint32_t gwAvailable() const {
    return kCredit - (gwSent - (up.creditDelivered() - gwReturnedBase));
  }

  // GatewayClient::handleFrame's producer path.
  Produced produce(uint32_t connection, uint32_t turn, uint32_t bytes) {
    if (ring.acceptedConnection() != connection) return Produced::WrongConnection;
    if (ring.freeBytes() < DownstreamRing::kHeaderBytes + bytes) {
      ++violations;
      return Produced::OverCapacity;
    }
    if (!credit.admit(bytes)) {
      ++violations;
      return Produced::OverCredit;
    }
    std::vector<uint8_t> frame(bytes);
    for (uint32_t i = 0; i < bytes / 2; ++i) {
      frame[2 * i] = static_cast<uint8_t>(nextSample & 0xFF);
      frame[2 * i + 1] = static_cast<uint8_t>(nextSample >> 8);
      ++nextSample;
    }
    TEST_ASSERT_TRUE(ring.write(connection, turn, frame.data(), bytes) == RingWrite::Written);
    gwSent += bytes;
    return Produced::Written;
  }

  AudioChunk chunk(uint32_t samples = 320) {
    AudioChunk c;
    c.samples = pcm;
    c.count = samples;
    c.format = monoS16(16000);
    return c;
  }

  // A whole user turn of `frames` 320-sample frames, sent completely.
  uint32_t upload(uint32_t frames = 2) {
    TEST_ASSERT_TRUE(source.beginUserTurn(monoS16(16000)));
    for (uint32_t i = 0; i < frames; ++i) {
      TEST_ASSERT_TRUE(source.pushUserAudio(chunk()) == PushResult::Accepted);
    }
    source.endUserTurn();
    up.send();
    source.poll(now);
    return source.activeTurn();
  }

  // PcmPlayer's side: peek, check the samples arrive in order, consume.
  uint32_t play(uint32_t maxSamples = 960, uint32_t maxChunks = 1000) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < maxChunks; ++i) {
      AudioChunk c;
      if (!source.peekPlaybackChunk(c, maxSamples)) break;
      TEST_ASSERT_EQUAL_UINT32(24000, c.format.sampleRate);
      for (uint32_t s = 0; s < c.count; ++s) {
        TEST_ASSERT_EQUAL_UINT16(expectedSample, static_cast<uint16_t>(c.samples[s]));
        ++expectedSample;
      }
      source.consumePlayback(c.count);
      total += c.count;
    }
    return total;
  }

  bool event(TurnEvent& out) { return source.nextEvent(out); }

  void expectEvent(TurnEventType type, TurnError error = TurnError::None) {
    TurnEvent e;
    TEST_ASSERT_TRUE_MESSAGE(source.nextEvent(e), "expected an event");
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(type), static_cast<uint8_t>(e.type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(error), static_cast<uint8_t>(e.error));
  }

  void expectNoEvent() {
    TurnEvent e;
    TEST_ASSERT_FALSE_MESSAGE(source.nextEvent(e), "unexpected event");
  }

  // Every granted byte is with the gateway, in the ring, consumed awaiting
  // return, or returned but not yet delivered.
  void expectConserved() {
    TEST_ASSERT_EQUAL_UINT32(gwSent, credit.received());
    const uint32_t awaiting = credit.consumedBytes() - credit.returned();
    const uint32_t inFlight = credit.returned() - (up.creditDelivered() - gwReturnedBase);
    TEST_ASSERT_EQUAL_UINT32(kCredit, gwAvailable() + credit.inRing() + awaiting + inFlight);
    TEST_ASSERT_EQUAL_UINT32(0, credit.overConsumed());
  }
};

}  // namespace

// --- a normal turn ------------------------------------------------------------------

static void a_turn_goes_out_in_order_and_completes_with_all_credit_returned() {
  Rig r;
  r.connect(1);
  TEST_ASSERT_TRUE(r.source.beginUserTurn(monoS16(16000)));
  const uint32_t turn = r.source.activeTurn();
  for (int i = 0; i < 3; ++i) {
    TEST_ASSERT_TRUE(r.source.pushUserAudio(r.chunk()) == PushResult::Accepted);
  }
  r.source.endUserTurn();
  TEST_ASSERT_TRUE(r.source.phase() == GatewayTurnSource::Phase::Awaiting);
  r.up.send();
  const std::vector<OutKind> kinds = r.up.kindsFor(turn);
  TEST_ASSERT_EQUAL_UINT32(5, kinds.size());
  TEST_ASSERT_TRUE(kinds[0] == OutKind::TurnStart);
  TEST_ASSERT_TRUE(kinds[1] == OutKind::Audio && kinds[2] == OutKind::Audio &&
                   kinds[3] == OutKind::Audio);
  TEST_ASSERT_TRUE(kinds[4] == OutKind::TurnEnd);
  TEST_ASSERT_EQUAL_UINT32(3u * 640u, r.source.upBytes());

  r.source.poll(r.now);
  TEST_ASSERT_TRUE(r.source.timerArmed());
  r.source.onControl(speechStart(turn), r.now);
  r.expectEvent(TurnEventType::SpeechStart);
  for (int i = 0; i < 3; ++i) TEST_ASSERT_TRUE(r.produce(1, turn, kFrame) == Produced::Written);
  // turn_complete can arrive before the audio is played: the turn completes
  // only once its bytes have been consumed.
  r.source.onControl(turnComplete(turn, 3, 3 * kFrame), r.now);
  r.expectNoEvent();
  r.expectConserved();
  TEST_ASSERT_EQUAL_UINT32(2880, r.play());
  r.expectEvent(TurnEventType::TurnComplete);
  TEST_ASSERT_TRUE(r.source.phase() == GatewayTurnSource::Phase::Idle);
  r.source.poll(r.now);
  r.up.send();
  TEST_ASSERT_EQUAL_UINT32(3 * kFrame, r.up.creditDelivered());
  TEST_ASSERT_EQUAL_UINT32(kCredit, r.gwAvailable());
  r.expectConserved();
  TEST_ASSERT_EQUAL_UINT32(1, r.source.counters().turnsCompleted);
  TEST_ASSERT_EQUAL_UINT32(0, r.source.counters().firstResponseTimeouts);
}

static void busy_never_advances_and_turn_end_counts_only_accepted_frames() {
  Rig r;
  r.connect(1);
  TEST_ASSERT_TRUE(r.source.beginUserTurn(monoS16(16000)));
  const uint32_t turn = r.source.activeTurn();
  r.up.busyAudio = true;
  TEST_ASSERT_TRUE(r.source.pushUserAudio(r.chunk()) == PushResult::Busy);
  TEST_ASSERT_EQUAL_UINT32(0, r.source.upFrames());
  r.up.busyAudio = false;
  // Fill the real queue until it pushes back.
  uint32_t accepted = 0;
  while (r.source.pushUserAudio(r.chunk()) == PushResult::Accepted) ++accepted;
  TEST_ASSERT_TRUE(accepted > 0 && accepted < OutboundQueue::kSlots);
  TEST_ASSERT_EQUAL_UINT32(accepted, r.source.upFrames());
  TEST_ASSERT_EQUAL_UINT32(2, r.source.counters().upstreamBusy);
  r.up.send();
  TEST_ASSERT_TRUE(r.source.pushUserAudio(r.chunk()) == PushResult::Accepted);
  r.source.endUserTurn();
  r.up.send();
  uint32_t audioOnWire = 0;
  for (OutKind k : r.up.kindsFor(turn)) {
    if (k == OutKind::Audio) ++audioOnWire;
  }
  TEST_ASSERT_EQUAL_UINT32(accepted + 1, audioOnWire);
  TEST_ASSERT_EQUAL_UINT32(audioOnWire, r.source.upFrames());
  TEST_ASSERT_TRUE(r.up.kindsFor(turn).back() == OutKind::TurnEnd);
}

static void turn_end_busy_is_retried_from_poll() {
  Rig r;
  r.connect(1);
  TEST_ASSERT_TRUE(r.source.beginUserTurn(monoS16(16000)));
  r.source.pushUserAudio(r.chunk());
  r.up.busyTurnEnd = true;
  r.source.endUserTurn();
  TEST_ASSERT_TRUE(r.source.phase() == GatewayTurnSource::Phase::Ending);
  r.source.poll(r.now);
  TEST_ASSERT_TRUE(r.source.phase() == GatewayTurnSource::Phase::Ending);
  // Pushing is not permitted once the user turn has ended.
  TEST_ASSERT_TRUE(r.source.pushUserAudio(r.chunk()) == PushResult::Fatal);
  r.up.busyTurnEnd = false;
  r.source.poll(r.now);
  TEST_ASSERT_TRUE(r.source.phase() == GatewayTurnSource::Phase::Awaiting);
  TEST_ASSERT_EQUAL_UINT32(2, r.source.counters().turnEndBusy);
}

// --- the first-response timeout -------------------------------------------------------

static void the_timeout_starts_only_when_turn_end_is_fully_written() {
  Rig r;
  r.connect(1);
  TEST_ASSERT_TRUE(r.source.beginUserTurn(monoS16(16000)));
  const uint32_t turn = r.source.activeTurn();
  r.source.pushUserAudio(r.chunk());
  r.source.endUserTurn();
  // Queued, not written: a minute passes without a timeout.
  r.now += 60000;
  r.source.poll(r.now);
  TEST_ASSERT_FALSE(r.source.timerArmed());
  r.expectNoEvent();
  // turn_end in flight but not complete: still not armed.
  r.up.send(2);  // turn_start, audio
  const OutboundItem* item = r.up.q.beginSend();
  TEST_ASSERT_TRUE(item != nullptr && item->kind == OutKind::TurnEnd);
  r.source.poll(r.now);
  TEST_ASSERT_FALSE(r.source.timerArmed());
  r.up.q.completeSend();
  r.up.lastEnd = turn;

  const uint32_t t0 = r.now;
  r.source.poll(t0);
  TEST_ASSERT_TRUE(r.source.timerArmed());
  r.source.poll(t0 + 9999);
  r.expectNoEvent();
  r.source.poll(t0 + 10000);
  r.expectEvent(TurnEventType::Error, TurnError::ResponseTimeout);
  r.source.poll(t0 + 20000);
  r.expectNoEvent();
  TEST_ASSERT_EQUAL_UINT32(1, r.source.counters().firstResponseTimeouts);
  TEST_ASSERT_EQUAL_UINT32(1, r.source.counters().turnsFailed);
  TEST_ASSERT_TRUE(r.source.phase() == GatewayTurnSource::Phase::Idle);
  // The gateway is told, and the connection is still usable.
  r.up.send();
  TEST_ASSERT_TRUE(r.up.wire.back().kind == OutKind::Cancel);
  TEST_ASSERT_EQUAL_UINT32(turn, r.up.wire.back().turn);
  TEST_ASSERT_TRUE(r.source.linkReady());
  r.upload();
  TEST_ASSERT_TRUE(r.source.phase() == GatewayTurnSource::Phase::Awaiting);
}

static void stale_events_and_stale_audio_do_not_satisfy_the_timeout() {
  Rig r;
  r.connect(1);
  const uint32_t old = r.upload();
  r.source.cancel();
  r.up.send();
  const uint32_t turn = r.upload();
  const uint32_t t0 = r.now;
  TEST_ASSERT_TRUE(r.source.timerArmed());

  r.source.onControl(speechStart(old), t0 + 100);
  r.source.onControl(speechStart(turn + 1000), t0 + 100);
  TEST_ASSERT_EQUAL_UINT32(2, r.source.counters().staleEvents);
  r.expectNoEvent();

  TEST_ASSERT_TRUE(r.produce(1, old, kFrame) == Produced::Written);
  r.source.poll(t0 + 200);
  TEST_ASSERT_EQUAL_UINT32(kFrame, r.source.counters().cancelledAudioBytes);
  TEST_ASSERT_EQUAL_UINT32(0, r.ring.usedBytes());
  // A frame written just before a connection switch: dropped, no credit.
  const uint8_t bytes[4] = {1, 2, 3, 4};
  r.ring.write(0, turn, bytes, 4);
  r.source.poll(t0 + 300);
  TEST_ASSERT_EQUAL_UINT32(4, r.source.counters().oldConnectionBytes);
  r.up.send();
  r.expectConserved();
  TEST_ASSERT_EQUAL_UINT32(kCredit, r.gwAvailable());

  r.source.poll(t0 + 10000);
  r.expectEvent(TurnEventType::Error, TurnError::ResponseTimeout);
}

static void a_valid_first_response_satisfies_the_timeout_for_good() {
  Rig r;
  r.connect(1);
  const uint32_t turn = r.upload();
  const uint32_t t0 = r.now;
  r.source.onControl(speechStart(turn), t0 + 700);
  r.expectEvent(TurnEventType::SpeechStart);
  TEST_ASSERT_TRUE(r.produce(1, turn, kFrame) == Produced::Written);
  r.source.poll(t0 + 750);
  TEST_ASSERT_EQUAL_UINT32(750, r.source.counters().lastFirstResponseMs);
  for (uint32_t t = 1000; t <= 60000; t += 1000) r.source.poll(t0 + t);
  r.expectNoEvent();
  TEST_ASSERT_TRUE(r.source.phase() == GatewayTurnSource::Phase::Responding);
}

static void speech_start_without_audio_still_times_out() {
  Rig r;
  r.connect(1);
  const uint32_t turn = r.upload();
  const uint32_t t0 = r.now;
  r.source.onControl(speechStart(turn), t0 + 100);
  r.expectEvent(TurnEventType::SpeechStart);
  r.source.poll(t0 + 9999);
  r.expectNoEvent();
  r.source.poll(t0 + 10000);
  r.expectEvent(TurnEventType::Error, TurnError::ResponseTimeout);
}

static void a_response_without_audio_completes_the_turn() {
  Rig r;
  r.connect(1);
  const uint32_t turn = r.upload();
  r.source.onControl(turnComplete(turn, 0, 0), r.now + 50);
  r.expectEvent(TurnEventType::TurnComplete);
  r.source.poll(r.now + 20000);
  r.expectNoEvent();
  TEST_ASSERT_EQUAL_UINT32(0, r.source.counters().firstResponseTimeouts);
}

static void audio_ahead_of_its_speech_start_is_held_not_dropped() {
  Rig r;
  r.connect(1);
  const uint32_t turn = r.upload();
  TEST_ASSERT_TRUE(r.produce(1, turn, kFrame) == Produced::Written);
  r.source.poll(r.now);
  TEST_ASSERT_EQUAL_UINT32(DownstreamRing::kHeaderBytes + kFrame, r.ring.usedBytes());
  TEST_ASSERT_EQUAL_UINT32(0, r.source.counters().staleAudioBytes);
  r.source.onControl(speechStart(turn), r.now);
  r.expectEvent(TurnEventType::SpeechStart);
  TEST_ASSERT_EQUAL_UINT32(960, r.play());
}

// --- cancel and barge-in --------------------------------------------------------------

static void barge_in_purges_returns_credit_and_the_cancel_precedes_the_next_turn() {
  Rig r;
  r.connect(1);
  const uint32_t turn = r.upload();
  r.source.onControl(speechStart(turn), r.now);
  r.expectEvent(TurnEventType::SpeechStart);
  for (int i = 0; i < 5; ++i) r.produce(1, turn, kFrame);
  TEST_ASSERT_EQUAL_UINT32(960, r.play(960, 1));
  TEST_ASSERT_EQUAL_UINT32(480, r.play(480, 1));  // half of the second frame
  r.expectConserved();

  r.source.cancel();
  TEST_ASSERT_TRUE(r.source.phase() == GatewayTurnSource::Phase::Idle);
  TEST_ASSERT_EQUAL_UINT32(0, r.ring.usedBytes());
  TEST_ASSERT_EQUAL_UINT32(0, r.credit.inRing());
  TEST_ASSERT_EQUAL_UINT32(5 * kFrame - 2880, r.source.counters().cancelledAudioBytes);
  TEST_ASSERT_EQUAL_UINT32(0, r.credit.pendingReturn(true));
  r.expectNoEvent();
  r.expectConserved();

  // Late audio and events of the cancelled turn: dropped, counted, returned.
  r.produce(1, turn, kFrame);
  r.source.poll(r.now);
  TEST_ASSERT_EQUAL_UINT32(6 * kFrame - 2880, r.source.counters().cancelledAudioBytes);
  r.source.onControl(speechStart(turn), r.now);
  r.source.onControl(turnComplete(turn, 6, 6 * kFrame), r.now);
  TEST_ASSERT_EQUAL_UINT32(2, r.source.counters().staleEvents);
  r.expectNoEvent();

  // The next turn starts before anything was sent: the cancel goes first.
  TEST_ASSERT_TRUE(r.source.beginUserTurn(monoS16(16000)));
  const uint32_t next = r.source.activeTurn();
  TEST_ASSERT_TRUE(next != turn);
  const size_t before = r.up.wire.size();
  r.up.send();
  bool cancelSeen = false;
  for (size_t i = before; i < r.up.wire.size(); ++i) {
    if (r.up.wire[i].kind == OutKind::Cancel && r.up.wire[i].turn == turn) cancelSeen = true;
    if (r.up.wire[i].kind == OutKind::TurnStart && r.up.wire[i].turn == next) {
      TEST_ASSERT_TRUE_MESSAGE(cancelSeen, "cancel(N) must precede turn_start(N+1)");
    }
  }
  TEST_ASSERT_TRUE(cancelSeen);
  TEST_ASSERT_EQUAL_UINT32(kCredit, r.gwAvailable());
  r.expectConserved();
}

static void a_cancel_never_overtakes_the_frame_in_flight() {
  Rig r;
  r.connect(1);
  TEST_ASSERT_TRUE(r.source.beginUserTurn(monoS16(16000)));
  const uint32_t turn = r.source.activeTurn();
  r.source.pushUserAudio(r.chunk());
  r.source.pushUserAudio(r.chunk());
  r.up.send(1);  // turn_start
  const OutboundItem* item = r.up.q.beginSend();  // first audio, partially written
  TEST_ASSERT_TRUE(item != nullptr && item->kind == OutKind::Audio);
  const uint32_t length = item->length;
  r.source.cancel();
  // The loop keeps working while the frame is on its way out.
  r.source.poll(r.now);
  TEST_ASSERT_TRUE(r.up.q.beginSend() == item);
  TEST_ASSERT_EQUAL_UINT32(length, item->length);
  r.up.q.completeSend();
  r.up.wire.push_back({OutKind::Audio, turn, 0});
  r.up.send();
  const std::vector<OutKind> kinds = r.up.kindsFor(turn);
  TEST_ASSERT_EQUAL_UINT32(3, kinds.size());
  TEST_ASSERT_TRUE(kinds[0] == OutKind::TurnStart);
  TEST_ASSERT_TRUE(kinds[1] == OutKind::Audio);
  TEST_ASSERT_TRUE(kinds[2] == OutKind::Cancel);
}

// --- session loss -----------------------------------------------------------------------

static void session_loss_while_listening_fails_once_and_the_next_turn_succeeds() {
  Rig r;
  r.connect(1);
  TEST_ASSERT_TRUE(r.source.beginUserTurn(monoS16(16000)));
  r.source.pushUserAudio(r.chunk());
  r.source.onSessionLost(r.now);
  r.expectEvent(TurnEventType::Error, TurnError::SessionLost);
  r.source.onSessionLost(r.now);
  r.source.poll(r.now + 20000);
  r.expectNoEvent();
  TEST_ASSERT_EQUAL_UINT32(1, r.source.counters().sessionLossFailures);
  TEST_ASSERT_FALSE(r.source.beginUserTurn(monoS16(16000)));

  r.connect(2);
  const uint32_t turn = r.upload();
  r.source.onControl(speechStart(turn), r.now);
  r.expectEvent(TurnEventType::SpeechStart);
  r.produce(2, turn, kFrame);
  r.source.onControl(turnComplete(turn, 1, kFrame), r.now);
  r.play();
  r.expectEvent(TurnEventType::TurnComplete);
  TEST_ASSERT_EQUAL_UINT32(1, r.source.counters().turnsCompleted);
}

static void session_loss_while_waiting_fails_once() {
  Rig r;
  r.connect(1);
  r.upload();
  r.source.onSessionLost(r.now);
  r.expectEvent(TurnEventType::Error, TurnError::SessionLost);
  // The timeout that was armed dies with the turn.
  r.source.poll(r.now + 30000);
  r.expectNoEvent();
  TEST_ASSERT_EQUAL_UINT32(0, r.source.counters().firstResponseTimeouts);
  TEST_ASSERT_EQUAL_UINT32(1, r.source.counters().turnsFailed);
}

static void session_loss_while_speaking_never_leaks_into_the_next_connection() {
  Rig r;
  r.connect(1);
  const uint32_t turn = r.upload();
  r.source.onControl(speechStart(turn), r.now);
  r.expectEvent(TurnEventType::SpeechStart);
  for (int i = 0; i < 3; ++i) r.produce(1, turn, kFrame);
  r.play(960, 1);
  r.source.onSessionLost(r.now);
  r.expectEvent(TurnEventType::Error, TurnError::SessionLost);
  TEST_ASSERT_EQUAL_UINT32(0, r.ring.usedBytes());
  // One more frame from connection 1 lands before the robot reconnects.
  const uint8_t late[4] = {9, 9, 9, 9};
  r.ring.write(1, turn, late, 4);

  r.connect(2);
  TEST_ASSERT_EQUAL_UINT32(4, r.source.counters().oldConnectionBytes);
  TEST_ASSERT_EQUAL_UINT32(0, r.credit.consumedBytes());
  TEST_ASSERT_EQUAL_UINT32(0, r.credit.returned());
  TEST_ASSERT_EQUAL_UINT32(kCredit, r.credit.gatewayRemaining());
  // Still arriving for connection 1: refused by the producer.
  TEST_ASSERT_TRUE(r.produce(1, turn, kFrame) == Produced::WrongConnection);

  const uint32_t next = r.upload();
  r.source.onControl(speechStart(next), r.now);
  r.expectEvent(TurnEventType::SpeechStart);
  r.produce(2, next, kFrame);
  r.source.onControl(turnComplete(next, 1, kFrame), r.now);
  r.expectedSample = r.nextSample - 960;
  r.play();
  r.expectEvent(TurnEventType::TurnComplete);
  r.source.poll(r.now);
  r.up.send();
  r.expectConserved();
  TEST_ASSERT_EQUAL_UINT32(0, r.violations);
}

static void reconnecting_with_a_turn_still_active_fails_it_once() {
  Rig r;
  r.connect(1);
  r.upload();
  r.connect(2);
  r.expectEvent(TurnEventType::Error, TurnError::SessionLost);
  r.expectNoEvent();
  TEST_ASSERT_EQUAL_UINT32(1, r.source.counters().sessionLossFailures);
}

// --- protocol ---------------------------------------------------------------------------

static void a_downstream_violation_fails_the_turn_once() {
  Rig r;
  r.connect(1);
  const uint32_t turn = r.upload();
  r.source.onControl(speechStart(turn), r.now);
  r.expectEvent(TurnEventType::SpeechStart);
  r.source.onDownstreamViolation(r.now);
  r.expectEvent(TurnEventType::Error, TurnError::Protocol);
  r.source.onDownstreamViolation(r.now);
  r.source.onSessionLost(r.now);
  r.expectNoEvent();
  TEST_ASSERT_EQUAL_UINT32(1, r.source.counters().turnsFailed);
}

static void a_gateway_error_fails_its_turn_and_a_connection_error_is_ignored() {
  Rig r;
  r.connect(1);
  const uint32_t turn = r.upload();
  wire::ControlMessage connectionError = control(wire::ControlType::Error, 0);
  connectionError.hasTurn = false;
  r.source.onControl(connectionError, r.now);
  r.expectNoEvent();
  r.source.onControl(control(wire::ControlType::Error, turn), r.now);
  r.expectEvent(TurnEventType::Error, TurnError::GatewayError);
  TEST_ASSERT_EQUAL_UINT32(1, r.source.counters().gatewayErrors);
}

static void turn_complete_totals_must_match_what_was_delivered() {
  {
    Rig r;  // declared more than delivered
    r.connect(1);
    const uint32_t turn = r.upload();
    r.source.onControl(speechStart(turn), r.now);
    r.expectEvent(TurnEventType::SpeechStart);
    r.produce(1, turn, kFrame);
    r.produce(1, turn, kFrame);
    r.source.onControl(turnComplete(turn, 3, 3 * kFrame), r.now);
    r.play();
    r.expectEvent(TurnEventType::Error, TurnError::Protocol);
  }
  {
    Rig r;  // declared less than delivered
    r.connect(1);
    const uint32_t turn = r.upload();
    r.source.onControl(speechStart(turn), r.now);
    r.expectEvent(TurnEventType::SpeechStart);
    for (int i = 0; i < 3; ++i) r.produce(1, turn, kFrame);
    r.source.onControl(turnComplete(turn, 2, 2 * kFrame), r.now);
    r.play();
    r.expectEvent(TurnEventType::Error, TurnError::Protocol);
    r.up.send();
    r.source.poll(r.now);
    r.up.send();
    r.expectConserved();
    TEST_ASSERT_EQUAL_UINT32(kCredit, r.gwAvailable());
  }
}

static void out_of_order_or_misformatted_responses_are_protocol_failures() {
  {
    Rig r;
    r.connect(1);
    const uint32_t turn = r.upload();
    r.source.onControl(speechStart(turn, "s16le/16000/1"), r.now);
    r.expectEvent(TurnEventType::Error, TurnError::Protocol);
  }
  {
    Rig r;
    r.connect(1);
    TEST_ASSERT_TRUE(r.source.beginUserTurn(monoS16(16000)));
    r.source.onControl(speechStart(r.source.activeTurn()), r.now);
    r.expectEvent(TurnEventType::Error, TurnError::Protocol);
  }
  {
    Rig r;
    r.connect(1);
    TEST_ASSERT_TRUE(r.source.beginUserTurn(monoS16(16000)));
    r.produce(1, r.source.activeTurn(), kFrame);
    r.source.poll(r.now);
    r.expectEvent(TurnEventType::Error, TurnError::Protocol);
    r.up.send();
    r.expectConserved();
  }
  {
    Rig r;  // wrong capture format never starts a turn
    r.connect(1);
    TEST_ASSERT_FALSE(r.source.beginUserTurn(monoS16(24000)));
  }
}

// --- credit over a long response --------------------------------------------------------

static void a_response_longer_than_the_ring_stalls_at_zero_credit_and_arrives_intact() {
  Rig r;
  r.connect(1);
  const uint32_t turn = r.upload();
  r.source.onControl(speechStart(turn), r.now);
  r.expectEvent(TurnEventType::SpeechStart);

  const uint32_t totalFrames = 250;  // 480 000 B = 10 s, 2.5x the ring
  uint32_t sentFrames = 0;
  uint32_t zeroStalls = 0;
  bool announced = false;
  bool completed = false;
  for (int step = 0; step < 100000 && !completed; ++step) {
    // Faster than real time: everything the credit allows, at once.
    while (sentFrames < totalFrames && r.gwAvailable() >= kFrame) {
      TEST_ASSERT_TRUE(r.produce(1, turn, kFrame) == Produced::Written);
      ++sentFrames;
      if (r.gwAvailable() == 0) ++zeroStalls;
    }
    r.expectConserved();
    if (sentFrames == totalFrames && !announced) {
      r.source.onControl(turnComplete(turn, totalFrames, totalFrames * kFrame), r.now);
      announced = true;
    }
    // The player takes two slots' worth per loop.
    r.play(960, 2);
    r.source.poll(r.now);
    r.up.send();
    r.expectConserved();
    TurnEvent e;
    while (r.event(e)) {
      TEST_ASSERT_TRUE(e.type == TurnEventType::TurnComplete);
      completed = true;
    }
    r.now += 20;
  }
  TEST_ASSERT_TRUE(completed);
  TEST_ASSERT_TRUE(zeroStalls > 10);
  TEST_ASSERT_EQUAL_UINT32(totalFrames * kFrame, r.source.lastResponseBytes());
  TEST_ASSERT_EQUAL_UINT32(0, r.violations);
  TEST_ASSERT_EQUAL_UINT32(0, r.credit.violations());
  TEST_ASSERT_TRUE(r.ring.highWaterBytes() <= kCredit + 100 * DownstreamRing::kHeaderBytes);
  TEST_ASSERT_EQUAL_UINT32(kCredit, r.gwAvailable());
}

static void a_frame_over_credit_or_capacity_is_refused_without_damage() {
  Rig r;
  r.connect(1);
  const uint32_t turn = r.upload();
  r.source.onControl(speechStart(turn), r.now);
  r.expectEvent(TurnEventType::SpeechStart);
  for (uint32_t i = 0; i < 100; ++i) r.produce(1, turn, kFrame);
  TEST_ASSERT_EQUAL_UINT32(0, r.credit.gatewayRemaining());
  const uint32_t used = r.ring.usedBytes();
  TEST_ASSERT_TRUE(r.produce(1, turn, kFrame) == Produced::OverCredit);
  TEST_ASSERT_EQUAL_UINT32(used, r.ring.usedBytes());
  TEST_ASSERT_EQUAL_UINT32(1, r.credit.violations());
  // GatewayClient reports it; the turn fails once and everything is returned.
  r.source.onDownstreamViolation(r.now);
  r.expectEvent(TurnEventType::Error, TurnError::Protocol);
  TEST_ASSERT_EQUAL_UINT32(0, r.ring.usedBytes());
  TEST_ASSERT_EQUAL_UINT32(0, r.credit.inRing());
  TEST_ASSERT_EQUAL_UINT32(100u * kFrame, r.credit.consumedBytes());
}

// --- the TurnBuffer lease under backpressure -------------------------------------------

static void the_turn_buffer_cannot_be_reset_while_backpressure_holds_unsent_audio() {
  Rig r;
  r.connect(1);
  std::vector<int16_t> storage(16000);
  TurnBuffer buffer;
  buffer.attach(storage.data(), 16000);
  std::vector<int16_t> samples(3200, 7);
  buffer.append(samples.data(), 3200);
  TurnStreamer streamer(320, 2);
  TEST_ASSERT_TRUE(streamer.begin(buffer, r.source, monoS16(16000)));

  r.up.busyAudio = true;
  for (int i = 0; i < 5; ++i) {
    TEST_ASSERT_TRUE(streamer.service(true) == StreamStatus::Streaming);
    TEST_ASSERT_FALSE(buffer.reset());
  }
  TEST_ASSERT_EQUAL_UINT32(0, streamer.cursor());
  TEST_ASSERT_EQUAL_UINT32(3200, buffer.committedSamples());

  r.up.busyAudio = false;
  StreamStatus status = StreamStatus::Streaming;
  for (int i = 0; i < 20 && status == StreamStatus::Streaming; ++i) {
    TEST_ASSERT_FALSE(buffer.reset());
    status = streamer.service(true);
    r.up.send();
  }
  TEST_ASSERT_TRUE(status == StreamStatus::Finished);
  TEST_ASSERT_EQUAL_UINT32(10, r.source.upFrames());
  TEST_ASSERT_TRUE(r.source.phase() == GatewayTurnSource::Phase::Awaiting);
  TEST_ASSERT_TRUE(buffer.reset());
}

// --- the loop and the sender, interleaved ------------------------------------------------

namespace {
struct Lcg {
  uint32_t state;
  explicit Lcg(uint32_t seed) : state(seed) {}
  uint32_t next() {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
  }
};
enum class WireState { Open, Ended, Cancelled };
}  // namespace

static void interleaved_loop_and_sender_never_reorder_or_split_a_turn() {
  Rig r;
  r.connect(1);
  Lcg rng(2026);
  const OutboundItem* inFlight = nullptr;
  Sent flight = {OutKind::Ping, 0, 0};
  uint32_t flightLength = 0;
  std::map<uint32_t, WireState> state;
  std::map<uint32_t, uint32_t> audio;
  std::map<uint32_t, uint32_t> expectedAudio;
  uint32_t ended = 0;

  for (int step = 0; step < 40000; ++step) {
    r.now += 1;
    switch (rng.next() % 7) {
      case 0:
        if (r.source.phase() == GatewayTurnSource::Phase::Idle) {
          r.source.beginUserTurn(monoS16(16000));
        }
        break;
      case 1:
        if (r.source.phase() == GatewayTurnSource::Phase::Uploading) {
          r.source.pushUserAudio(r.chunk(1 + rng.next() % 320));
        }
        break;
      case 2:
        if (r.source.phase() == GatewayTurnSource::Phase::Uploading && rng.next() % 4 == 0) {
          const uint32_t turn = r.source.activeTurn();
          r.source.endUserTurn();
          expectedAudio[turn] = r.source.upFrames();
        }
        break;
      case 3:
        if (r.source.phase() != GatewayTurnSource::Phase::Idle && rng.next() % 10 == 0) {
          r.source.cancel();
        }
        break;
      case 4:
        if (inFlight == nullptr) {
          inFlight = r.up.q.beginSend();
          if (inFlight != nullptr) {
            flight = {inFlight->kind, inFlight->turn, inFlight->value};
            flightLength = inFlight->length;
          }
        }
        break;
      case 5:
        if (inFlight != nullptr) {
          // The frame being written was not touched by anything the loop did.
          TEST_ASSERT_TRUE(inFlight->kind == flight.kind);
          TEST_ASSERT_EQUAL_UINT32(flight.turn, inFlight->turn);
          if (flight.kind != OutKind::Credit) TEST_ASSERT_EQUAL_UINT32(flightLength, inFlight->length);
          r.up.q.completeSend();
          inFlight = nullptr;
          if (flight.kind == OutKind::TurnEnd) r.up.lastEnd = flight.turn;
          const uint32_t t = flight.turn;
          switch (flight.kind) {
            case OutKind::TurnStart:
              TEST_ASSERT_TRUE(state.find(t) == state.end());
              state[t] = WireState::Open;
              break;
            case OutKind::Audio:
              TEST_ASSERT_TRUE(state[t] == WireState::Open);
              ++audio[t];
              break;
            case OutKind::TurnEnd:
              TEST_ASSERT_TRUE(state[t] == WireState::Open);
              TEST_ASSERT_EQUAL_UINT32(expectedAudio[t], audio[t]);
              state[t] = WireState::Ended;
              ++ended;
              break;
            case OutKind::Cancel:
              TEST_ASSERT_TRUE(state[t] == WireState::Open || state[t] == WireState::Ended);
              state[t] = WireState::Cancelled;
              break;
            default:
              break;
          }
        }
        break;
      case 6:
        if (r.source.phase() == GatewayTurnSource::Phase::Awaiting && rng.next() % 3 == 0) {
          r.source.onControl(turnComplete(r.source.activeTurn(), 0, 0), r.now);
        }
        break;
    }
    r.source.poll(r.now);
    TurnEvent e;
    while (r.event(e)) {}
  }
  TEST_ASSERT_TRUE(ended > 50);
  TEST_ASSERT_TRUE(state.size() > 100);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(a_turn_goes_out_in_order_and_completes_with_all_credit_returned);
  RUN_TEST(busy_never_advances_and_turn_end_counts_only_accepted_frames);
  RUN_TEST(turn_end_busy_is_retried_from_poll);
  RUN_TEST(the_timeout_starts_only_when_turn_end_is_fully_written);
  RUN_TEST(stale_events_and_stale_audio_do_not_satisfy_the_timeout);
  RUN_TEST(a_valid_first_response_satisfies_the_timeout_for_good);
  RUN_TEST(speech_start_without_audio_still_times_out);
  RUN_TEST(a_response_without_audio_completes_the_turn);
  RUN_TEST(audio_ahead_of_its_speech_start_is_held_not_dropped);
  RUN_TEST(barge_in_purges_returns_credit_and_the_cancel_precedes_the_next_turn);
  RUN_TEST(a_cancel_never_overtakes_the_frame_in_flight);
  RUN_TEST(session_loss_while_listening_fails_once_and_the_next_turn_succeeds);
  RUN_TEST(session_loss_while_waiting_fails_once);
  RUN_TEST(session_loss_while_speaking_never_leaks_into_the_next_connection);
  RUN_TEST(reconnecting_with_a_turn_still_active_fails_it_once);
  RUN_TEST(a_downstream_violation_fails_the_turn_once);
  RUN_TEST(a_gateway_error_fails_its_turn_and_a_connection_error_is_ignored);
  RUN_TEST(turn_complete_totals_must_match_what_was_delivered);
  RUN_TEST(out_of_order_or_misformatted_responses_are_protocol_failures);
  RUN_TEST(a_response_longer_than_the_ring_stalls_at_zero_credit_and_arrives_intact);
  RUN_TEST(a_frame_over_credit_or_capacity_is_refused_without_damage);
  RUN_TEST(the_turn_buffer_cannot_be_reset_while_backpressure_holds_unsent_audio);
  RUN_TEST(interleaved_loop_and_sender_never_reorder_or_split_a_turn);
  return UNITY_END();
}
