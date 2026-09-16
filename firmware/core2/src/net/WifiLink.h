#pragma once

#include <stdint.h>

#include "tth/AppState.h"
#include "tth/DeviceConfig.h"
#include "tth/LinkStateMachine.h"

namespace tth {

// The Wi-Fi station, driven by LinkStateMachine (Step 6.1).
//
// Polled from the cooperative loop; it registers NO callbacks, so the Wi-Fi
// event task can never call into App. Link state is read from WiFi.status(),
// a cached value.
//
// Two library defaults are overridden, deliberately:
//   * WiFi.persistent(false): otherwise the Arduino Wi-Fi library copies the
//     SSID and password into the Wi-Fi driver's OWN NVS namespace, where a
//     TTH Bot reset could not erase them. Credentials live only in "tth".
//   * WiFi.setAutoReconnect(false): reconnection, with backoff and logging,
//     belongs to LinkStateMachine.
//
// No gateway connection here — that is Step 6.2.
class WifiLink {
 public:
  WifiLink();

  // Once, before the loop.
  void begin();

  // A new configuration (null = not provisioned). Starts or stops the link.
  // The Wi-Fi driver is started on first use, so an unprovisioned robot never
  // spends memory on it.
  void apply(const config::DeviceConfig* config, uint32_t nowMs);

  // Once per loop. Non-blocking apart from occasional driver calls, which are
  // timed and flagged via consumeTransition().
  void poll(uint32_t nowMs);

  Connectivity connectivity() const { return _machine.connectivity(); }
  LinkState state() const { return _machine.state(); }
  uint32_t attempt() const { return _machine.attempt(); }
  uint32_t lastDelayMs() const { return _machine.lastDelayMs(); }
  uint32_t connects() const { return _machine.connects(); }
  int rssi() const;

  // True once after a loop iteration did driver work (start, connect,
  // disconnect), so App can count that iteration as a transition.
  bool consumeTransition();

 private:
  void execute(LinkAction action, uint32_t nowMs);
  bool startDriver();
  void stopDriver();

  LinkStateMachine _machine;
  config::DeviceConfig _config;
  bool _hasConfig;
  bool _driverStarted;
  bool _transition;
  bool _m1Reported;
  uint32_t _attemptStartedMs;
  uint32_t _connectedAtMs;
};

}  // namespace tth
