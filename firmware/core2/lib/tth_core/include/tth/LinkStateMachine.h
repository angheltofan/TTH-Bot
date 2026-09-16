#pragma once

#include <stdint.h>

#include "tth/AppState.h"

// The Wi-Fi link state machine (PHASE6_PLAN §8, Step 6.1).
//
//   Unprovisioned --start(provisioned)--> Connecting
//   Connecting    --connected-----------> Connected     (failures reset)
//   Connecting    --no link in timeout--> Backoff(n)    -> Disconnect
//   Connected     --link lost-----------> Backoff(0)    -> Disconnect
//   Backoff       --delay elapsed-------> Connecting    -> StartConnect
//   any           --start(false)--------> Unprovisioned -> Disconnect
//
// Backoff: 1, 2, 4, 8, 16, then 30 s, each with ±20 % jitter from a seedable
// generator, so a room full of robots does not retry in lockstep.
//
// It decides; it never touches the radio. The device adapter (WifiLink)
// executes the returned action. Portable and clock-injected.

namespace tth {

enum class LinkState : uint8_t { Unprovisioned = 0, Connecting, Connected, Backoff };
enum class LinkAction : uint8_t { None = 0, StartConnect, Disconnect };
enum class LinkFailure : uint8_t { None = 0, ConnectTimeout, LinkLost };

const char* toString(LinkState state);
const char* toString(LinkFailure failure);

class Backoff {
 public:
  static const uint32_t kSteps = 6;
  // The Wi-Fi schedule (the default).
  static const uint32_t kScheduleMs[kSteps];

  explicit Backoff(uint32_t seed);
  // Another schedule (for example the gateway's 2..60 s). Not copied: it must
  // outlive this object.
  Backoff(uint32_t seed, const uint32_t* scheduleMs, uint32_t steps);
  void reseed(uint32_t seed);

  // The delay before retry number `attempt` (0-based), jittered ±20 %. The
  // last step repeats.
  uint32_t delayMs(uint32_t attempt);

 private:
  uint32_t _state;
  const uint32_t* _schedule;
  uint32_t _steps;
};

class LinkStateMachine {
 public:
  LinkStateMachine(uint32_t connectTimeoutMs, uint32_t seed);

  void reseed(uint32_t seed) { _backoff.reseed(seed); }

  // (Re)starts from the configuration: provisioned -> connect now, failures
  // cleared; otherwise stop.
  LinkAction start(bool provisioned, uint32_t nowMs);

  LinkAction poll(uint32_t nowMs, bool wifiConnected);

  LinkState state() const { return _state; }
  Connectivity connectivity() const;
  uint32_t attempt() const { return _attempt; }  // consecutive failures
  uint32_t lastDelayMs() const { return _lastDelayMs; }
  uint32_t retryAtMs() const { return _retryAtMs; }
  uint32_t sinceMs() const { return _sinceMs; }
  LinkFailure lastFailure() const { return _lastFailure; }
  uint32_t connects() const { return _connects; }

 private:
  LinkAction enterBackoff(uint32_t nowMs, LinkFailure why);

  const uint32_t _connectTimeoutMs;
  Backoff _backoff;
  LinkState _state;
  uint32_t _attempt;
  uint32_t _lastDelayMs;
  uint32_t _retryAtMs;
  uint32_t _sinceMs;
  LinkFailure _lastFailure;
  uint32_t _connects;
};

}  // namespace tth
