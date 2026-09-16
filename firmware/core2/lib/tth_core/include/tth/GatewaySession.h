#pragma once

#include <stdint.h>

#include "tth/AppState.h"
#include "tth/GatewayProtocol.h"
#include "tth/LinkStateMachine.h"

// The gateway session state machine (PHASE6_PLAN §8, D1, D4; Step 6.2).
//
//   Idle ──network up + target──▶ Connecting ──WebSocket open──▶ AwaitingReady
//     ▲                              │  (hello sent)                  │ ready
//     │                              ▼                                ▼
//     └──network lost──── Closing ◀── failure / timeout ────────── Ready
//                           │                                      (ping/pong)
//                           ▼
//   Backoff 2, 4, 8, 16, 32, then 60 s (±20 %) ──▶ Connecting
//
//   HTTP 401/403          ──▶ AuthRejected: ERROR face, retry every 5 min only
//   HTTP 400/404/426/…,
//   a bad 101, wrong `out`,
//   error{protocol}       ──▶ ProtocolMismatch: ERROR face, no retry until the
//                              configuration changes or the robot reboots
//
// Proactive reconnect (D1): at session age >= 55 min the session is closed and
// reopened AS SOON AS THE CONVERSATION IS IDLE -- never mid-turn. No new turn
// may start at >= 58 min, or after the gateway's session_end{max_age}.
//
// It decides; it never touches a socket. The device adapter (GatewayClient)
// executes the returned actions and reports what happened. Portable and
// clock-injected.

namespace tth {

enum class SessionState : uint8_t {
  Idle = 0,
  Backoff,
  Connecting,     // TCP + TLS + WebSocket upgrade in progress
  AwaitingReady,  // hello sent, waiting for ready
  Ready,
  Closing,
  AuthRejected,
  ProtocolMismatch,
};

enum class SessionAction : uint8_t { None = 0, Connect, SendHello, SendPing, Close };

enum class ConnectFailure : uint8_t {
  None = 0,
  Dns,
  Network,            // TCP, handshake timeout, upgrade timeout
  Tls,                // certificate or TLS protocol failure
  TlsAlloc,           // mbedTLS could not allocate (counted for PHASE6_PLAN §7)
  Unauthorized,       // HTTP 401 / 403
  Throttled,          // HTTP 429
  ServerUnavailable,  // HTTP 503
  ServerError,        // other 5xx
  Rejected,           // any other non-101 status: wrong URL or protocol
  BadHandshake,       // 101 without the right accept key, upgrade or subprotocol
};

enum class SessionEnd : uint8_t {
  None = 0,
  ConnectFailed,
  ConnectTimeout,
  ReadyTimeout,
  PongTimeout,
  PeerClosed,
  ProtocolError,
  GatewayError,
  Replaced,
  MaxAge,
  Proactive,
  NetworkLost,
  Reconfigured,
};

const char* toString(SessionState state);
const char* toString(SessionAction action);
const char* toString(ConnectFailure failure);
const char* toString(SessionEnd end);

// The failure an HTTP status (other than 101) stands for.
ConnectFailure failureForHttpStatus(uint16_t status);

struct SessionTimings {
  uint32_t connectTimeoutMs;  // watchdog over TCP + TLS + upgrade
  uint32_t readyTimeoutMs;    // hello -> ready (the gateway waits up to 15 s for Gemini)
  uint32_t pingIntervalMs;
  uint32_t pongTimeoutMs;
  uint32_t closeTimeoutMs;
  uint32_t authRetryMs;
  uint32_t proactiveReconnectMs;
  uint32_t turnGuardMs;
};

// 45 s, 25 s, 15 s, 10 s, 5 s, 5 min, 55 min, 58 min.
SessionTimings defaultSessionTimings();

class GatewaySession {
 public:
  static const uint32_t kBackoffSteps = 6;
  static const uint32_t kBackoffScheduleMs[kBackoffSteps];

  GatewaySession(const SessionTimings& timings, uint32_t seed);

  void reseed(uint32_t seed) { _backoff.reseed(seed); }

  // A new configuration. `haveTarget` = gateway URL, identity and a CA are all
  // provisioned. Clears AuthRejected / ProtocolMismatch and the failure count.
  void configure(bool haveTarget, uint32_t nowMs);

  // The Wi-Fi link. Losing it closes the session; regaining it reconnects at
  // once (the gateway did nothing wrong).
  void setNetworkUp(bool up, uint32_t nowMs);

  // --- reports from the transport ---------------------------------------------
  void onConnected(uint32_t nowMs);
  void onConnectFailed(ConnectFailure failure, uint32_t nowMs);
  void onControl(const wire::ControlMessage& message, uint32_t nowMs);
  void onControlError(uint32_t nowMs);  // a malformed control frame
  void onClosed(uint32_t nowMs);

  // What to do now. Call until it returns None. `conversationIdle` gates the
  // proactive reconnect.
  SessionAction poll(uint32_t nowMs, bool conversationIdle);

  // False when no new turn may begin (not ready, >= 58 min, or max_age).
  bool mayStartTurn(uint32_t nowMs) const;

  Connectivity connectivity() const;
  SessionState state() const { return _state; }

  uint32_t failures() const { return _failures; }
  uint32_t lastDelayMs() const { return _lastDelayMs; }
  uint32_t retryAtMs() const { return _retryAtMs; }
  SessionEnd lastEnd() const { return _lastEnd; }
  ConnectFailure lastFailure() const { return _lastFailure; }
  bool rejected() const { return _rejected; }
  uint32_t readyCount() const { return _readyCount; }
  uint32_t lastReadyLatencyMs() const { return _lastReadyLatencyMs; }
  uint32_t sessionAgeMs(uint32_t nowMs) const;
  uint32_t pingTs() const { return _pingTs; }
  uint32_t pingsSent() const { return _pingsSent; }
  uint32_t lastRttMs() const { return _lastRttMs; }
  uint32_t maxRttMs() const { return _maxRttMs; }
  uint32_t strayPongs() const { return _strayPongs; }
  uint32_t unknownMessages() const { return _unknownMessages; }
  uint32_t unexpectedMessages() const { return _unexpectedMessages; }
  uint32_t protocolErrors() const { return _protocolErrors; }
  uint32_t turnErrors() const { return _turnErrors; }
  bool maxAgeRequested() const { return _maxAgeRequested; }
  const char* sessionId() const { return _sessionId; }
  const char* activityId() const { return _activityId; }
  const char* lastErrorCode() const { return _lastErrorCode; }

 private:
  enum class After : uint8_t { Backoff, ReconnectNow, Mismatch, Idle };

  void startConnect(uint32_t nowMs);
  void scheduleBackoff(uint32_t nowMs);
  void enterAuthRejected(uint32_t nowMs);
  void beginClose(SessionEnd why, After after, uint32_t nowMs);
  void afterClosed(uint32_t nowMs);
  SessionAction take();

  const SessionTimings _t;
  Backoff _backoff;
  SessionState _state;
  After _after;
  SessionAction _pending;
  bool _haveTarget;
  bool _networkUp;
  bool _rejected;  // the last completed connect attempt was a 401/403
  uint32_t _failures;
  uint32_t _lastDelayMs;
  uint32_t _retryAtMs;
  uint32_t _connectStartedMs;
  uint32_t _readyDeadlineMs;
  uint32_t _readyAtMs;
  uint32_t _closeStartedMs;
  uint32_t _nextPingAtMs;
  uint32_t _pingSentAtMs;
  uint32_t _pingTs;
  bool _awaitingPong;
  bool _maxAgeRequested;
  SessionEnd _lastEnd;
  ConnectFailure _lastFailure;
  uint32_t _readyCount;
  uint32_t _lastReadyLatencyMs;
  uint32_t _lastRttMs;
  uint32_t _maxRttMs;
  uint32_t _pingsSent;
  uint32_t _strayPongs;
  uint32_t _unknownMessages;
  uint32_t _unexpectedMessages;
  uint32_t _protocolErrors;
  uint32_t _turnErrors;
  char _sessionId[40];
  char _activityId[40];
  char _lastErrorCode[32];
};

}  // namespace tth
