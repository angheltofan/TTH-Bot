#include "tth/GatewaySession.h"

#include <string.h>

namespace tth {

const uint32_t GatewaySession::kBackoffScheduleMs[GatewaySession::kBackoffSteps] = {
    2000, 4000, 8000, 16000, 32000, 60000};

namespace {

const char* const kExpectedDownFormat = "s16le/24000/1";

bool due(uint32_t nowMs, uint32_t atMs) {
  return static_cast<int32_t>(nowMs - atMs) >= 0;
}

void copyText(char* dest, size_t capacity, const char* source) {
  size_t n = strlen(source);
  if (n + 1 > capacity) n = capacity - 1;
  memcpy(dest, source, n);
  dest[n] = '\0';
}

}  // namespace

const char* toString(SessionState state) {
  switch (state) {
    case SessionState::Idle:
      return "idle";
    case SessionState::Backoff:
      return "backoff";
    case SessionState::Connecting:
      return "connecting";
    case SessionState::AwaitingReady:
      return "awaiting-ready";
    case SessionState::Ready:
      return "ready";
    case SessionState::Closing:
      return "closing";
    case SessionState::AuthRejected:
      return "auth-rejected";
    case SessionState::ProtocolMismatch:
      return "protocol-mismatch";
  }
  return "invalid";
}

const char* toString(SessionAction action) {
  switch (action) {
    case SessionAction::None:
      return "none";
    case SessionAction::Connect:
      return "connect";
    case SessionAction::SendHello:
      return "send-hello";
    case SessionAction::SendPing:
      return "send-ping";
    case SessionAction::Close:
      return "close";
  }
  return "invalid";
}

const char* toString(ConnectFailure failure) {
  switch (failure) {
    case ConnectFailure::None:
      return "none";
    case ConnectFailure::Dns:
      return "dns lookup failed";
    case ConnectFailure::Network:
      return "network or timeout";
    case ConnectFailure::Tls:
      return "tls failure (certificate or protocol)";
    case ConnectFailure::TlsAlloc:
      return "tls allocation failure";
    case ConnectFailure::Unauthorized:
      return "unauthorized";
    case ConnectFailure::Throttled:
      return "throttled";
    case ConnectFailure::ServerUnavailable:
      return "gateway unavailable";
    case ConnectFailure::ServerError:
      return "gateway error";
    case ConnectFailure::Rejected:
      return "request rejected";
    case ConnectFailure::BadHandshake:
      return "bad websocket handshake";
  }
  return "invalid";
}

const char* toString(SessionEnd end) {
  switch (end) {
    case SessionEnd::None:
      return "none";
    case SessionEnd::ConnectFailed:
      return "connect failed";
    case SessionEnd::ConnectTimeout:
      return "connect timeout";
    case SessionEnd::ReadyTimeout:
      return "no ready in time";
    case SessionEnd::PongTimeout:
      return "no pong in time";
    case SessionEnd::PeerClosed:
      return "closed by the gateway";
    case SessionEnd::ProtocolError:
      return "protocol error";
    case SessionEnd::GatewayError:
      return "gateway error message";
    case SessionEnd::Replaced:
      return "replaced by a newer connection";
    case SessionEnd::MaxAge:
      return "gateway max age";
    case SessionEnd::Proactive:
      return "proactive reconnect";
    case SessionEnd::NetworkLost:
      return "wi-fi lost";
    case SessionEnd::Reconfigured:
      return "reconfigured";
  }
  return "invalid";
}

ConnectFailure failureForHttpStatus(uint16_t status) {
  if (status == 401 || status == 403) return ConnectFailure::Unauthorized;
  if (status == 429) return ConnectFailure::Throttled;
  if (status == 503) return ConnectFailure::ServerUnavailable;
  if (status >= 500 && status <= 599) return ConnectFailure::ServerError;
  return ConnectFailure::Rejected;
}

SessionTimings defaultSessionTimings() {
  SessionTimings t;
  t.connectTimeoutMs = 45000;
  t.readyTimeoutMs = 25000;
  t.pingIntervalMs = 15000;
  t.pongTimeoutMs = 10000;
  t.closeTimeoutMs = 5000;
  t.authRetryMs = 300000;
  t.proactiveReconnectMs = 55u * 60000u;
  t.turnGuardMs = 58u * 60000u;
  return t;
}

GatewaySession::GatewaySession(const SessionTimings& timings, uint32_t seed)
    : _t(timings),
      _backoff(seed, kBackoffScheduleMs, kBackoffSteps),
      _state(SessionState::Idle),
      _after(After::Backoff),
      _pending(SessionAction::None),
      _haveTarget(false),
      _networkUp(false),
      _rejected(false),
      _failures(0),
      _lastDelayMs(0),
      _retryAtMs(0),
      _connectStartedMs(0),
      _readyDeadlineMs(0),
      _readyAtMs(0),
      _closeStartedMs(0),
      _nextPingAtMs(0),
      _pingSentAtMs(0),
      _pingTs(0),
      _awaitingPong(false),
      _maxAgeRequested(false),
      _lastEnd(SessionEnd::None),
      _lastFailure(ConnectFailure::None),
      _readyCount(0),
      _lastReadyLatencyMs(0),
      _lastRttMs(0),
      _maxRttMs(0),
      _pingsSent(0),
      _strayPongs(0),
      _unknownMessages(0),
      _unexpectedMessages(0),
      _protocolErrors(0),
      _turnErrors(0) {
  _sessionId[0] = '\0';
  _activityId[0] = '\0';
  _lastErrorCode[0] = '\0';
}

SessionAction GatewaySession::take() {
  const SessionAction action = _pending;
  _pending = SessionAction::None;
  return action;
}

void GatewaySession::startConnect(uint32_t nowMs) {
  _state = SessionState::Connecting;
  _connectStartedMs = nowMs;
  _awaitingPong = false;
  _pending = SessionAction::Connect;
}

void GatewaySession::scheduleBackoff(uint32_t nowMs) {
  if (!_haveTarget || !_networkUp) {
    _state = SessionState::Idle;
    return;
  }
  _lastDelayMs = _backoff.delayMs(_failures);
  ++_failures;
  _retryAtMs = nowMs + _lastDelayMs;
  _state = SessionState::Backoff;
}

void GatewaySession::enterAuthRejected(uint32_t nowMs) {
  _rejected = true;
  ++_failures;
  _lastDelayMs = _t.authRetryMs;
  _retryAtMs = nowMs + _t.authRetryMs;
  _state = SessionState::AuthRejected;
}

void GatewaySession::beginClose(SessionEnd why, After after, uint32_t nowMs) {
  _lastEnd = why;
  _after = after;
  _state = SessionState::Closing;
  _closeStartedMs = nowMs;
  _awaitingPong = false;
  _pending = SessionAction::Close;
}

void GatewaySession::afterClosed(uint32_t nowMs) {
  switch (_after) {
    case After::Backoff:
      scheduleBackoff(nowMs);
      return;
    case After::ReconnectNow:
    case After::Idle:
      _state = SessionState::Idle;
      if (_haveTarget && _networkUp) startConnect(nowMs);
      return;
    case After::Mismatch:
      _state = SessionState::ProtocolMismatch;
      return;
  }
}

void GatewaySession::configure(bool haveTarget, uint32_t nowMs) {
  const bool active = _state == SessionState::Connecting ||
                      _state == SessionState::AwaitingReady ||
                      _state == SessionState::Ready || _state == SessionState::Closing;
  _haveTarget = haveTarget;
  _rejected = false;
  _failures = 0;
  _maxAgeRequested = false;
  _lastFailure = ConnectFailure::None;
  if (active) {
    beginClose(SessionEnd::Reconfigured, After::Idle, nowMs);
    return;
  }
  _state = SessionState::Idle;
  _pending = SessionAction::None;
  if (_haveTarget && _networkUp) startConnect(nowMs);
}

void GatewaySession::setNetworkUp(bool up, uint32_t nowMs) {
  if (up == _networkUp) return;
  _networkUp = up;
  if (!up) {
    switch (_state) {
      case SessionState::Connecting:
      case SessionState::AwaitingReady:
      case SessionState::Ready:
        beginClose(SessionEnd::NetworkLost, After::Idle, nowMs);
        return;
      case SessionState::Backoff:
        _state = SessionState::Idle;
        return;
      case SessionState::Closing:
        if (_after != After::Mismatch) _after = After::Idle;
        return;
      case SessionState::Idle:
      case SessionState::AuthRejected:
      case SessionState::ProtocolMismatch:
        return;
    }
    return;
  }
  if (_state == SessionState::Idle && _haveTarget) {
    _failures = 0;
    startConnect(nowMs);
  }
}

void GatewaySession::onConnected(uint32_t nowMs) {
  if (_state != SessionState::Connecting) return;
  _state = SessionState::AwaitingReady;
  _readyDeadlineMs = nowMs + _t.readyTimeoutMs;
  _pending = SessionAction::SendHello;
}

void GatewaySession::onConnectFailed(ConnectFailure failure, uint32_t nowMs) {
  if (_state == SessionState::Closing) {
    afterClosed(nowMs);
    return;
  }
  if (_state != SessionState::Connecting) return;
  _lastFailure = failure;
  _lastEnd = SessionEnd::ConnectFailed;
  switch (failure) {
    case ConnectFailure::Unauthorized:
      enterAuthRejected(nowMs);
      return;
    case ConnectFailure::Rejected:
    case ConnectFailure::BadHandshake:
      _state = SessionState::ProtocolMismatch;
      return;
    default:
      scheduleBackoff(nowMs);
      return;
  }
}

void GatewaySession::onControl(const wire::ControlMessage& message, uint32_t nowMs) {
  if (_state != SessionState::AwaitingReady && _state != SessionState::Ready) return;

  switch (message.type) {
    case wire::ControlType::Ready:
      if (_state == SessionState::Ready) {
        ++_protocolErrors;
        beginClose(SessionEnd::ProtocolError, After::Backoff, nowMs);
        return;
      }
      if (strcmp(message.format, kExpectedDownFormat) != 0) {
        ++_protocolErrors;
        beginClose(SessionEnd::ProtocolError, After::Mismatch, nowMs);
        return;
      }
      _state = SessionState::Ready;
      _readyAtMs = nowMs;
      ++_readyCount;
      _failures = 0;
      _rejected = false;
      _lastFailure = ConnectFailure::None;
      _lastReadyLatencyMs = nowMs - _connectStartedMs;
      _nextPingAtMs = nowMs + _t.pingIntervalMs;
      _awaitingPong = false;
      _maxAgeRequested = false;
      copyText(_sessionId, sizeof(_sessionId), message.session);
      copyText(_activityId, sizeof(_activityId), message.activity);
      return;

    case wire::ControlType::Pong:
      if (_state == SessionState::Ready && _awaitingPong && message.ts == _pingTs) {
        _awaitingPong = false;
        _lastRttMs = nowMs - _pingSentAtMs;
        if (_lastRttMs > _maxRttMs) _maxRttMs = _lastRttMs;
      } else {
        ++_strayPongs;
      }
      return;

    case wire::ControlType::Error:
      copyText(_lastErrorCode, sizeof(_lastErrorCode), message.code);
      if (message.hasTurn) {
        // Turn-scoped: Step 6.3 acts on it. It never ends the session.
        ++_turnErrors;
        return;
      }
      if (!message.retry && strcmp(message.code, "protocol") == 0) {
        beginClose(SessionEnd::GatewayError, After::Mismatch, nowMs);
        return;
      }
      beginClose(SessionEnd::GatewayError, After::Backoff, nowMs);
      return;

    case wire::ControlType::SessionEnd:
      if (strcmp(message.reason, "max_age") == 0) {
        // Reconnect as soon as the conversation is idle; no new turn first.
        _maxAgeRequested = true;
        return;
      }
      beginClose(strcmp(message.reason, "replaced") == 0 ? SessionEnd::Replaced
                                                        : SessionEnd::PeerClosed,
                 After::Backoff, nowMs);
      return;

    case wire::ControlType::SpeechStart:
    case wire::ControlType::TurnComplete:
    case wire::ControlType::Interrupted:
      // Turn-scoped (Step 6.3): the gateway turn source acts on them. They
      // never change the session.
      return;

    case wire::ControlType::ActivityList:
    case wire::ControlType::ActivitySelected:
    case wire::ControlType::ActivitySelectError:
      // Activity selection: the application's selector acts on them. They
      // never change the session.
      return;

    case wire::ControlType::Unknown:
      ++_unknownMessages;
      return;
  }
}

void GatewaySession::onControlError(uint32_t nowMs) {
  if (_state != SessionState::AwaitingReady && _state != SessionState::Ready) return;
  ++_protocolErrors;
  beginClose(SessionEnd::ProtocolError, After::Backoff, nowMs);
}

void GatewaySession::onClosed(uint32_t nowMs) {
  switch (_state) {
    case SessionState::Closing:
      afterClosed(nowMs);
      return;
    case SessionState::AwaitingReady:
    case SessionState::Ready:
      _lastEnd = SessionEnd::PeerClosed;
      _awaitingPong = false;
      scheduleBackoff(nowMs);
      return;
    case SessionState::Connecting:
      _lastFailure = ConnectFailure::Network;
      _lastEnd = SessionEnd::ConnectFailed;
      scheduleBackoff(nowMs);
      return;
    case SessionState::Idle:
    case SessionState::Backoff:
    case SessionState::AuthRejected:
    case SessionState::ProtocolMismatch:
      return;
  }
}

SessionAction GatewaySession::poll(uint32_t nowMs, bool conversationIdle) {
  if (_pending != SessionAction::None) return take();

  switch (_state) {
    case SessionState::Backoff:
    case SessionState::AuthRejected:
      if (_networkUp && _haveTarget && due(nowMs, _retryAtMs)) {
        startConnect(nowMs);
        return take();
      }
      return SessionAction::None;

    case SessionState::Connecting:
      if (nowMs - _connectStartedMs >= _t.connectTimeoutMs) {
        _lastFailure = ConnectFailure::Network;
        beginClose(SessionEnd::ConnectTimeout, After::Backoff, nowMs);
        return take();
      }
      return SessionAction::None;

    case SessionState::AwaitingReady:
      if (due(nowMs, _readyDeadlineMs)) {
        beginClose(SessionEnd::ReadyTimeout, After::Backoff, nowMs);
        return take();
      }
      return SessionAction::None;

    case SessionState::Ready:
      if (_awaitingPong && nowMs - _pingSentAtMs >= _t.pongTimeoutMs) {
        beginClose(SessionEnd::PongTimeout, After::Backoff, nowMs);
        return take();
      }
      if (conversationIdle &&
          (_maxAgeRequested || nowMs - _readyAtMs >= _t.proactiveReconnectMs)) {
        beginClose(_maxAgeRequested ? SessionEnd::MaxAge : SessionEnd::Proactive,
                   After::ReconnectNow, nowMs);
        return take();
      }
      if (!_awaitingPong && due(nowMs, _nextPingAtMs)) {
        _pingTs = nowMs;
        _pingSentAtMs = nowMs;
        _awaitingPong = true;
        _nextPingAtMs = nowMs + _t.pingIntervalMs;
        ++_pingsSent;
        return SessionAction::SendPing;
      }
      return SessionAction::None;

    case SessionState::Closing:
      if (nowMs - _closeStartedMs >= _t.closeTimeoutMs) {
        afterClosed(nowMs);
        return take();
      }
      return SessionAction::None;

    case SessionState::Idle:
    case SessionState::ProtocolMismatch:
      return SessionAction::None;
  }
  return SessionAction::None;
}

bool GatewaySession::mayStartTurn(uint32_t nowMs) const {
  return _state == SessionState::Ready && !_maxAgeRequested &&
         (nowMs - _readyAtMs) < _t.turnGuardMs;
}

uint32_t GatewaySession::sessionAgeMs(uint32_t nowMs) const {
  return _state == SessionState::Ready ? nowMs - _readyAtMs : 0;
}

Connectivity GatewaySession::connectivity() const {
  if (_state == SessionState::Ready) return Connectivity::Online;
  if (_state == SessionState::AuthRejected ||
      _state == SessionState::ProtocolMismatch || _rejected) {
    return Connectivity::Failed;
  }
  if (_state == SessionState::Closing && _after == After::Mismatch) {
    return Connectivity::Failed;
  }
  if (_state == SessionState::Connecting || _state == SessionState::AwaitingReady ||
      _state == SessionState::Closing) {
    return Connectivity::Connecting;
  }
  return Connectivity::Offline;
}

}  // namespace tth
