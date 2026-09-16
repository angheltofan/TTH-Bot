// Host-side tests for the gateway session state machine: connect, hello,
// ready, keepalive, backoff, the persistent AuthRejected / ProtocolMismatch
// states, session_end, and the proactive reconnect between turns only.

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "tth/GatewaySession.h"

using tth::Connectivity;
using tth::ConnectFailure;
using tth::GatewaySession;
using tth::SessionAction;
using tth::SessionEnd;
using tth::SessionState;

void setUp() {}
void tearDown() {}

namespace {

const char* const kReady =
    "{\"t\":\"ready\",\"session\":\"s-1\",\"activity\":\"a-1\",\"out\":\"s16le/24000/1\"}";

tth::wire::ControlMessage message(const char* json) {
  tth::wire::ControlMessage m;
  const tth::wire::ControlError error = tth::wire::parseControl(json, strlen(json), m);
  TEST_ASSERT_TRUE_MESSAGE(error == tth::wire::ControlError::None, json);
  return m;
}

tth::wire::ControlMessage pong(uint32_t ts) {
  char json[64];
  snprintf(json, sizeof(json), "{\"t\":\"pong\",\"ts\":%lu}", static_cast<unsigned long>(ts));
  return message(json);
}

void assertJitter(uint32_t baseMs, uint32_t actualMs) {
  TEST_ASSERT_TRUE_MESSAGE(actualMs >= (baseMs * 8u) / 10u, "below -20 %");
  TEST_ASSERT_TRUE_MESSAGE(actualMs <= (baseMs * 12u) / 10u, "above +20 %");
}

struct Rig {
  GatewaySession s;
  uint32_t now;

  Rig() : s(tth::defaultSessionTimings(), 42), now(1000) {}

  SessionAction poll(bool idle = true) { return s.poll(now, idle); }

  void connect() {
    s.configure(true, now);
    s.setNetworkUp(true, now);
    TEST_ASSERT_TRUE(poll() == SessionAction::Connect);
  }

  void open() {
    connect();
    s.onConnected(now);
    TEST_ASSERT_TRUE(poll() == SessionAction::SendHello);
  }

  void ready() {
    open();
    s.onControl(message(kReady), now);
    TEST_ASSERT_TRUE(s.state() == SessionState::Ready);
  }

  // Advances in 1 s steps, answering every ping at once. Returns the first
  // action other than a ping, or None.
  SessionAction run(uint32_t ms, bool idle) {
    const uint32_t end = now + ms;
    while (now < end) {
      now += 1000;
      for (int i = 0; i < 4; ++i) {
        const SessionAction action = poll(idle);
        if (action == SessionAction::None) break;
        if (action != SessionAction::SendPing) return action;
        s.onControl(pong(s.pingTs()), now);
      }
    }
    return SessionAction::None;
  }
};

}  // namespace

static void without_a_target_or_network_nothing_happens() {
  Rig r;
  r.s.configure(false, r.now);
  r.s.setNetworkUp(true, r.now);
  for (int i = 0; i < 600; ++i) {
    r.now += 1000;
    TEST_ASSERT_TRUE(r.poll() == SessionAction::None);
  }
  TEST_ASSERT_TRUE(r.s.connectivity() == Connectivity::Offline);

  Rig q;
  q.s.configure(true, q.now);  // network still down
  TEST_ASSERT_TRUE(q.poll() == SessionAction::None);
  TEST_ASSERT_TRUE(q.s.state() == SessionState::Idle);
}

static void connect_hello_and_ready() {
  Rig r;
  r.connect();
  TEST_ASSERT_TRUE(r.s.connectivity() == Connectivity::Connecting);
  r.now += 1500;
  r.s.onConnected(r.now);
  TEST_ASSERT_TRUE(r.poll() == SessionAction::SendHello);
  TEST_ASSERT_TRUE(r.s.state() == SessionState::AwaitingReady);
  TEST_ASSERT_TRUE(r.s.connectivity() == Connectivity::Connecting);
  TEST_ASSERT_FALSE(r.s.mayStartTurn(r.now));
  r.now += 300;
  r.s.onControl(message(kReady), r.now);
  TEST_ASSERT_TRUE(r.s.state() == SessionState::Ready);
  TEST_ASSERT_TRUE(r.s.connectivity() == Connectivity::Online);
  TEST_ASSERT_EQUAL_UINT32(1, r.s.readyCount());
  TEST_ASSERT_EQUAL_UINT32(1800, r.s.lastReadyLatencyMs());
  TEST_ASSERT_EQUAL_STRING("s-1", r.s.sessionId());
  TEST_ASSERT_EQUAL_STRING("a-1", r.s.activityId());
  TEST_ASSERT_TRUE(r.s.mayStartTurn(r.now));
  TEST_ASSERT_TRUE(r.poll() == SessionAction::None);
}

static void pings_every_interval_and_measures_rtt() {
  Rig r;
  r.ready();
  const uint32_t readyAt = r.now;
  r.now = readyAt + 14999;
  TEST_ASSERT_TRUE(r.poll() == SessionAction::None);
  r.now = readyAt + 15000;
  TEST_ASSERT_TRUE(r.poll() == SessionAction::SendPing);
  TEST_ASSERT_EQUAL_UINT32(r.now, r.s.pingTs());
  r.now += 40;
  r.s.onControl(pong(r.s.pingTs()), r.now);
  TEST_ASSERT_EQUAL_UINT32(40, r.s.lastRttMs());
  r.now += 5000;
  TEST_ASSERT_TRUE(r.poll() == SessionAction::None);
  r.s.onControl(pong(12345), r.now);
  TEST_ASSERT_EQUAL_UINT32(1, r.s.strayPongs());
  r.now = readyAt + 30000;
  TEST_ASSERT_TRUE(r.poll() == SessionAction::SendPing);
  TEST_ASSERT_EQUAL_UINT32(2, r.s.pingsSent());
}

static void a_missing_pong_reconnects_with_backoff() {
  Rig r;
  r.ready();
  r.now += 15000;
  TEST_ASSERT_TRUE(r.poll() == SessionAction::SendPing);
  r.now += 9999;
  TEST_ASSERT_TRUE(r.poll() == SessionAction::None);
  r.now += 1;
  TEST_ASSERT_TRUE(r.poll() == SessionAction::Close);
  TEST_ASSERT_TRUE(r.s.state() == SessionState::Closing);
  r.s.onClosed(r.now);
  TEST_ASSERT_TRUE(r.s.state() == SessionState::Backoff);
  TEST_ASSERT_TRUE(r.s.lastEnd() == SessionEnd::PongTimeout);
  TEST_ASSERT_EQUAL_UINT32(1, r.s.failures());
  assertJitter(2000, r.s.lastDelayMs());
  TEST_ASSERT_TRUE(r.s.connectivity() == Connectivity::Offline);
  r.now += r.s.lastDelayMs() - 1;
  TEST_ASSERT_TRUE(r.poll() == SessionAction::None);
  r.now += 1;
  TEST_ASSERT_TRUE(r.poll() == SessionAction::Connect);
}

// PHASE6_PLAN §8: 2, 4, 8, 16, 32, then 60 s, each ±20 %.
static void repeated_failures_back_off_2_to_60_seconds() {
  Rig r;
  r.connect();
  const uint32_t expected[8] = {2000, 4000, 8000, 16000, 32000, 60000, 60000, 60000};
  for (int i = 0; i < 8; ++i) {
    r.s.onConnectFailed(ConnectFailure::Network, r.now);
    TEST_ASSERT_TRUE(r.s.state() == SessionState::Backoff);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(i + 1), r.s.failures());
    assertJitter(expected[i], r.s.lastDelayMs());
    r.now += r.s.lastDelayMs();
    TEST_ASSERT_TRUE(r.poll() == SessionAction::Connect);
  }
}

static void a_ready_session_resets_the_failures() {
  Rig r;
  r.connect();
  for (int i = 0; i < 3; ++i) {
    r.s.onConnectFailed(ConnectFailure::Tls, r.now);
    r.now += r.s.lastDelayMs();
    TEST_ASSERT_TRUE(r.poll() == SessionAction::Connect);
  }
  r.s.onConnected(r.now);
  TEST_ASSERT_TRUE(r.poll() == SessionAction::SendHello);
  r.s.onControl(message(kReady), r.now);
  TEST_ASSERT_EQUAL_UINT32(0, r.s.failures());
  r.now += 1000;
  r.s.onClosed(r.now);
  TEST_ASSERT_TRUE(r.s.lastEnd() == SessionEnd::PeerClosed);
  assertJitter(2000, r.s.lastDelayMs());
}

// D4: AuthRejected is a persistent ERROR, retried every 5 min only.
static void unauthorized_is_persistent_and_retried_every_five_minutes() {
  Rig r;
  r.connect();
  r.s.onConnectFailed(ConnectFailure::Unauthorized, r.now);
  TEST_ASSERT_TRUE(r.s.state() == SessionState::AuthRejected);
  TEST_ASSERT_TRUE(r.s.connectivity() == Connectivity::Failed);
  TEST_ASSERT_TRUE(r.s.rejected());
  TEST_ASSERT_EQUAL_UINT32(300000, r.s.lastDelayMs());
  const uint32_t start = r.now;
  r.now = start + 299999;
  TEST_ASSERT_TRUE(r.poll() == SessionAction::None);
  r.now = start + 300000;
  TEST_ASSERT_TRUE(r.poll() == SessionAction::Connect);
  TEST_ASSERT_TRUE(r.s.connectivity() == Connectivity::Failed);  // still an error while retrying
  r.s.onConnectFailed(ConnectFailure::Network, r.now);
  TEST_ASSERT_TRUE(r.s.state() == SessionState::Backoff);
  TEST_ASSERT_TRUE(r.s.connectivity() == Connectivity::Failed);
  r.now += r.s.lastDelayMs();
  TEST_ASSERT_TRUE(r.poll() == SessionAction::Connect);
  r.s.onConnected(r.now);
  TEST_ASSERT_TRUE(r.poll() == SessionAction::SendHello);
  r.s.onControl(message(kReady), r.now);
  TEST_ASSERT_TRUE(r.s.connectivity() == Connectivity::Online);
  TEST_ASSERT_FALSE(r.s.rejected());
}

// D4: ProtocolMismatch waits for a reboot or a new configuration.
static void a_protocol_mismatch_is_never_retried() {
  const ConnectFailure failures[2] = {ConnectFailure::Rejected, ConnectFailure::BadHandshake};
  for (int f = 0; f < 2; ++f) {
    Rig r;
    r.connect();
    r.s.onConnectFailed(failures[f], r.now);
    TEST_ASSERT_TRUE(r.s.state() == SessionState::ProtocolMismatch);
    TEST_ASSERT_TRUE(r.s.connectivity() == Connectivity::Failed);
    for (int i = 0; i < 120; ++i) {
      r.now += 60000;
      TEST_ASSERT_TRUE(r.poll() == SessionAction::None);
    }
    r.s.configure(true, r.now);
    TEST_ASSERT_TRUE(r.poll() == SessionAction::Connect);
  }
}

static void http_statuses_map_to_failures() {
  TEST_ASSERT_TRUE(tth::failureForHttpStatus(401) == ConnectFailure::Unauthorized);
  TEST_ASSERT_TRUE(tth::failureForHttpStatus(403) == ConnectFailure::Unauthorized);
  TEST_ASSERT_TRUE(tth::failureForHttpStatus(429) == ConnectFailure::Throttled);
  TEST_ASSERT_TRUE(tth::failureForHttpStatus(503) == ConnectFailure::ServerUnavailable);
  TEST_ASSERT_TRUE(tth::failureForHttpStatus(500) == ConnectFailure::ServerError);
  TEST_ASSERT_TRUE(tth::failureForHttpStatus(404) == ConnectFailure::Rejected);
  TEST_ASSERT_TRUE(tth::failureForHttpStatus(400) == ConnectFailure::Rejected);
  TEST_ASSERT_TRUE(tth::failureForHttpStatus(426) == ConnectFailure::Rejected);
  // Throttling and unavailability back off; they are not persistent.
  Rig r;
  r.connect();
  r.s.onConnectFailed(ConnectFailure::Throttled, r.now);
  TEST_ASSERT_TRUE(r.s.state() == SessionState::Backoff);
}

static void no_ready_in_time_reconnects() {
  Rig r;
  r.open();
  r.now += 24999;
  TEST_ASSERT_TRUE(r.poll() == SessionAction::None);
  r.now += 1;
  TEST_ASSERT_TRUE(r.poll() == SessionAction::Close);
  r.s.onClosed(r.now);
  TEST_ASSERT_TRUE(r.s.state() == SessionState::Backoff);
  TEST_ASSERT_TRUE(r.s.lastEnd() == SessionEnd::ReadyTimeout);
}

static void a_ready_with_the_wrong_audio_format_is_a_mismatch() {
  Rig r;
  r.open();
  r.s.onControl(message("{\"t\":\"ready\",\"session\":\"s\",\"activity\":\"a\","
                        "\"out\":\"s16le/16000/1\"}"),
                r.now);
  TEST_ASSERT_TRUE(r.poll() == SessionAction::Close);
  TEST_ASSERT_TRUE(r.s.connectivity() == Connectivity::Failed);
  r.s.onClosed(r.now);
  TEST_ASSERT_TRUE(r.s.state() == SessionState::ProtocolMismatch);
}

static void a_malformed_control_frame_reconnects() {
  Rig r;
  r.ready();
  r.s.onControlError(r.now);
  TEST_ASSERT_TRUE(r.poll() == SessionAction::Close);
  r.s.onClosed(r.now);
  TEST_ASSERT_TRUE(r.s.state() == SessionState::Backoff);
  TEST_ASSERT_TRUE(r.s.lastEnd() == SessionEnd::ProtocolError);
  TEST_ASSERT_EQUAL_UINT32(1, r.s.protocolErrors());
}

static void gateway_error_messages() {
  {
    Rig r;
    r.open();
    r.s.onControl(message("{\"t\":\"error\",\"code\":\"gemini_unavailable\",\"retry\":true}"),
                  r.now);
    TEST_ASSERT_TRUE(r.poll() == SessionAction::Close);
    r.s.onClosed(r.now);
    TEST_ASSERT_TRUE(r.s.state() == SessionState::Backoff);
    TEST_ASSERT_EQUAL_STRING("gemini_unavailable", r.s.lastErrorCode());
  }
  {
    Rig r;
    r.ready();
    r.s.onControl(message("{\"t\":\"error\",\"code\":\"protocol\",\"retry\":false}"), r.now);
    TEST_ASSERT_TRUE(r.poll() == SessionAction::Close);
    TEST_ASSERT_TRUE(r.s.connectivity() == Connectivity::Failed);
    r.s.onClosed(r.now);
    TEST_ASSERT_TRUE(r.s.state() == SessionState::ProtocolMismatch);
  }
  {
    Rig r;
    r.ready();
    r.s.onControl(message("{\"t\":\"error\",\"code\":\"bad_turn\",\"retry\":false,\"turn\":3}"),
                  r.now);
    TEST_ASSERT_TRUE(r.s.state() == SessionState::Ready);
    TEST_ASSERT_EQUAL_UINT32(1, r.s.turnErrors());
    TEST_ASSERT_TRUE(r.poll() == SessionAction::None);
  }
}

static void session_end_replaced_and_max_age() {
  {
    Rig r;
    r.ready();
    r.s.onControl(message("{\"t\":\"session_end\",\"reason\":\"replaced\"}"), r.now);
    TEST_ASSERT_TRUE(r.poll() == SessionAction::Close);
    r.s.onClosed(r.now);
    TEST_ASSERT_TRUE(r.s.state() == SessionState::Backoff);
    TEST_ASSERT_TRUE(r.s.lastEnd() == SessionEnd::Replaced);
  }
  {
    Rig r;
    r.ready();
    r.s.onControl(message("{\"t\":\"session_end\",\"reason\":\"max_age\"}"), r.now);
    TEST_ASSERT_TRUE(r.s.state() == SessionState::Ready);
    TEST_ASSERT_FALSE(r.s.mayStartTurn(r.now));
    r.now += 1000;
    TEST_ASSERT_TRUE(r.poll(false) == SessionAction::None);  // mid-turn: wait
    TEST_ASSERT_TRUE(r.poll(true) == SessionAction::Close);
    TEST_ASSERT_TRUE(r.s.lastEnd() == SessionEnd::MaxAge);
    r.s.onClosed(r.now);
    TEST_ASSERT_TRUE(r.poll() == SessionAction::Connect);  // at once, no backoff
    TEST_ASSERT_EQUAL_UINT32(0, r.s.failures());
  }
}

// D1: reconnect at >= 55 min, but only between turns; no new turn at >= 58 min.
static void the_proactive_reconnect_waits_for_an_idle_conversation() {
  Rig r;
  r.ready();
  TEST_ASSERT_TRUE(r.run(55u * 60000u - 1000u, true) == SessionAction::None);
  TEST_ASSERT_TRUE(r.run(1000, false) == SessionAction::None);  // 55 min, but busy
  TEST_ASSERT_TRUE(r.s.mayStartTurn(r.now));
  TEST_ASSERT_TRUE(r.run(3u * 60000u - 1000u, false) == SessionAction::None);
  TEST_ASSERT_TRUE(r.s.mayStartTurn(r.now));  // 57:59
  TEST_ASSERT_TRUE(r.run(1000, false) == SessionAction::None);
  TEST_ASSERT_FALSE(r.s.mayStartTurn(r.now));  // 58:00
  TEST_ASSERT_TRUE(r.poll(true) == SessionAction::Close);
  TEST_ASSERT_TRUE(r.s.lastEnd() == SessionEnd::Proactive);
  r.s.onClosed(r.now);
  TEST_ASSERT_TRUE(r.poll() == SessionAction::Connect);
  TEST_ASSERT_EQUAL_UINT32(0, r.s.failures());
}

// Step 6.3: turn messages belong to the turn source; the session neither
// counts them as unexpected nor changes state.
static void turn_messages_are_left_to_the_turn_source() {
  Rig r;
  r.ready();
  r.s.onControl(message("{\"t\":\"speech_start\",\"turn\":1,\"fmt\":\"s16le/24000/1\"}"),
                r.now);
  r.s.onControl(message("{\"t\":\"future_thing\"}"), r.now);
  TEST_ASSERT_EQUAL_UINT32(0, r.s.unexpectedMessages());
  TEST_ASSERT_EQUAL_UINT32(1, r.s.unknownMessages());
  TEST_ASSERT_TRUE(r.s.state() == SessionState::Ready);
}

static void losing_wi_fi_closes_and_regaining_it_reconnects_at_once() {
  {
    Rig r;
    r.ready();
    r.s.setNetworkUp(false, r.now);
    TEST_ASSERT_TRUE(r.poll() == SessionAction::Close);
    TEST_ASSERT_TRUE(r.s.lastEnd() == SessionEnd::NetworkLost);
    r.s.onClosed(r.now);
    TEST_ASSERT_TRUE(r.s.state() == SessionState::Idle);
    TEST_ASSERT_TRUE(r.poll() == SessionAction::None);
    r.now += 60000;
    r.s.setNetworkUp(true, r.now);
    TEST_ASSERT_TRUE(r.poll() == SessionAction::Connect);
  }
  {
    Rig r;
    r.connect();
    r.s.onConnectFailed(ConnectFailure::Network, r.now);
    r.s.setNetworkUp(false, r.now);
    TEST_ASSERT_TRUE(r.s.state() == SessionState::Idle);
    r.s.setNetworkUp(true, r.now);
    TEST_ASSERT_TRUE(r.poll() == SessionAction::Connect);
    TEST_ASSERT_EQUAL_UINT32(0, r.s.failures());
  }
}

static void reconfiguring_restarts_the_session() {
  {
    Rig r;
    r.ready();
    r.s.configure(true, r.now);
    TEST_ASSERT_TRUE(r.poll() == SessionAction::Close);
    TEST_ASSERT_TRUE(r.s.lastEnd() == SessionEnd::Reconfigured);
    r.s.onClosed(r.now);
    TEST_ASSERT_TRUE(r.poll() == SessionAction::Connect);
  }
  {
    Rig r;
    r.ready();
    r.s.configure(false, r.now);
    TEST_ASSERT_TRUE(r.poll() == SessionAction::Close);
    r.s.onClosed(r.now);
    TEST_ASSERT_TRUE(r.s.state() == SessionState::Idle);
    TEST_ASSERT_TRUE(r.poll() == SessionAction::None);
  }
  {
    // Reconfigured while the connect is still in progress: its failure report
    // completes the close.
    Rig r;
    r.connect();
    r.s.configure(true, r.now);
    TEST_ASSERT_TRUE(r.poll() == SessionAction::Close);
    r.s.onConnectFailed(ConnectFailure::Network, r.now);
    TEST_ASSERT_TRUE(r.poll() == SessionAction::Connect);
  }
}

static void watchdogs_recover_from_missing_reports() {
  {
    Rig r;
    r.ready();
    r.s.configure(true, r.now);
    TEST_ASSERT_TRUE(r.poll() == SessionAction::Close);
    r.now += 4999;
    TEST_ASSERT_TRUE(r.poll() == SessionAction::None);
    r.now += 1;
    TEST_ASSERT_TRUE(r.poll() == SessionAction::Connect);  // no close report came
  }
  {
    Rig r;
    r.connect();
    r.now += 44999;
    TEST_ASSERT_TRUE(r.poll() == SessionAction::None);
    r.now += 1;
    TEST_ASSERT_TRUE(r.poll() == SessionAction::Close);
    TEST_ASSERT_TRUE(r.s.lastEnd() == SessionEnd::ConnectTimeout);
    r.s.onConnectFailed(ConnectFailure::Network, r.now);
    TEST_ASSERT_TRUE(r.s.state() == SessionState::Backoff);
  }
}

static void reports_in_the_wrong_state_are_ignored() {
  Rig r;
  r.s.onConnected(r.now);
  r.s.onClosed(r.now);
  r.s.onControl(message(kReady), r.now);
  r.s.onControlError(r.now);
  TEST_ASSERT_TRUE(r.s.state() == SessionState::Idle);
  TEST_ASSERT_TRUE(r.poll() == SessionAction::None);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(without_a_target_or_network_nothing_happens);
  RUN_TEST(connect_hello_and_ready);
  RUN_TEST(pings_every_interval_and_measures_rtt);
  RUN_TEST(a_missing_pong_reconnects_with_backoff);
  RUN_TEST(repeated_failures_back_off_2_to_60_seconds);
  RUN_TEST(a_ready_session_resets_the_failures);
  RUN_TEST(unauthorized_is_persistent_and_retried_every_five_minutes);
  RUN_TEST(a_protocol_mismatch_is_never_retried);
  RUN_TEST(http_statuses_map_to_failures);
  RUN_TEST(no_ready_in_time_reconnects);
  RUN_TEST(a_ready_with_the_wrong_audio_format_is_a_mismatch);
  RUN_TEST(a_malformed_control_frame_reconnects);
  RUN_TEST(gateway_error_messages);
  RUN_TEST(session_end_replaced_and_max_age);
  RUN_TEST(the_proactive_reconnect_waits_for_an_idle_conversation);
  RUN_TEST(turn_messages_are_left_to_the_turn_source);
  RUN_TEST(losing_wi_fi_closes_and_regaining_it_reconnects_at_once);
  RUN_TEST(reconfiguring_restarts_the_session);
  RUN_TEST(watchdogs_recover_from_missing_reports);
  RUN_TEST(reports_in_the_wrong_state_are_ignored);
  return UNITY_END();
}
