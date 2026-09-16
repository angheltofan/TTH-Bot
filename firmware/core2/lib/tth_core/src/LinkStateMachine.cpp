#include "tth/LinkStateMachine.h"

namespace tth {

const uint32_t Backoff::kScheduleMs[Backoff::kSteps] = {1000, 2000, 4000,
                                                        8000, 16000, 30000};

const char* toString(LinkState state) {
  switch (state) {
    case LinkState::Unprovisioned:
      return "unprovisioned";
    case LinkState::Connecting:
      return "connecting";
    case LinkState::Connected:
      return "connected";
    case LinkState::Backoff:
      return "backoff";
  }
  return "invalid";
}

const char* toString(LinkFailure failure) {
  switch (failure) {
    case LinkFailure::None:
      return "none";
    case LinkFailure::ConnectTimeout:
      return "connect timeout";
    case LinkFailure::LinkLost:
      return "link lost";
  }
  return "invalid";
}

Backoff::Backoff(uint32_t seed)
    : _state(seed == 0 ? 1u : seed), _schedule(kScheduleMs), _steps(kSteps) {}

Backoff::Backoff(uint32_t seed, const uint32_t* scheduleMs, uint32_t steps)
    : _state(seed == 0 ? 1u : seed), _schedule(scheduleMs), _steps(steps) {}

void Backoff::reseed(uint32_t seed) { _state = (seed == 0) ? 1u : seed; }

uint32_t Backoff::delayMs(uint32_t attempt) {
  if (_schedule == nullptr || _steps == 0) return 0;
  const uint32_t last = _steps - 1;
  const uint32_t base = _schedule[attempt < last ? attempt : last];
  // xorshift32
  _state ^= _state << 13;
  _state ^= _state >> 17;
  _state ^= _state << 5;
  const uint32_t permille = 800u + (_state % 401u);  // 0.800 .. 1.200
  return static_cast<uint32_t>((static_cast<uint64_t>(base) * permille) / 1000u);
}

LinkStateMachine::LinkStateMachine(uint32_t connectTimeoutMs, uint32_t seed)
    : _connectTimeoutMs(connectTimeoutMs),
      _backoff(seed),
      _state(LinkState::Unprovisioned),
      _attempt(0),
      _lastDelayMs(0),
      _retryAtMs(0),
      _sinceMs(0),
      _lastFailure(LinkFailure::None),
      _connects(0) {}

Connectivity LinkStateMachine::connectivity() const {
  switch (_state) {
    case LinkState::Connected:
      return Connectivity::Online;
    case LinkState::Connecting:
      return Connectivity::Connecting;
    case LinkState::Backoff:
    case LinkState::Unprovisioned:
      return Connectivity::Offline;
  }
  return Connectivity::Offline;
}

LinkAction LinkStateMachine::start(bool provisioned, uint32_t nowMs) {
  _attempt = 0;
  _lastFailure = LinkFailure::None;
  _sinceMs = nowMs;
  if (!provisioned) {
    const bool wasUnprovisioned = (_state == LinkState::Unprovisioned);
    _state = LinkState::Unprovisioned;
    return wasUnprovisioned ? LinkAction::None : LinkAction::Disconnect;
  }
  _state = LinkState::Connecting;
  return LinkAction::StartConnect;
}

LinkAction LinkStateMachine::enterBackoff(uint32_t nowMs, LinkFailure why) {
  _lastDelayMs = _backoff.delayMs(_attempt);
  ++_attempt;
  _retryAtMs = nowMs + _lastDelayMs;
  _sinceMs = nowMs;
  _lastFailure = why;
  _state = LinkState::Backoff;
  return LinkAction::Disconnect;
}

LinkAction LinkStateMachine::poll(uint32_t nowMs, bool wifiConnected) {
  switch (_state) {
    case LinkState::Unprovisioned:
      return LinkAction::None;

    case LinkState::Connecting:
      if (wifiConnected) {
        _state = LinkState::Connected;
        _sinceMs = nowMs;
        _attempt = 0;
        _lastFailure = LinkFailure::None;
        ++_connects;
        return LinkAction::None;
      }
      if (nowMs - _sinceMs >= _connectTimeoutMs) {
        return enterBackoff(nowMs, LinkFailure::ConnectTimeout);
      }
      return LinkAction::None;

    case LinkState::Connected:
      if (!wifiConnected) {
        _attempt = 0;
        return enterBackoff(nowMs, LinkFailure::LinkLost);
      }
      return LinkAction::None;

    case LinkState::Backoff:
      if (static_cast<int32_t>(nowMs - _retryAtMs) >= 0) {
        _state = LinkState::Connecting;
        _sinceMs = nowMs;
        return LinkAction::StartConnect;
      }
      return LinkAction::None;
  }
  return LinkAction::None;
}

}  // namespace tth
