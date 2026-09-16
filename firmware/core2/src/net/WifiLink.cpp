#include "net/WifiLink.h"

#include <WiFi.h>
#include <esp_system.h>

#include "diag/BlockTimer.h"
#include "diag/SerialLog.h"
#include "diag/StartupReport.h"
#include "tth/Config.h"

namespace tth {

namespace {

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "idle";
    case WL_NO_SSID_AVAIL:
      return "network not found";
    case WL_SCAN_COMPLETED:
      return "scan completed";
    case WL_CONNECTED:
      return "connected";
    case WL_CONNECT_FAILED:
      return "connect failed";
    case WL_CONNECTION_LOST:
      return "connection lost";
    case WL_DISCONNECTED:
      return "disconnected";
    case WL_NO_SHIELD:
      return "no radio";
  }
  return "unknown";
}

}  // namespace

WifiLink::WifiLink()
    : _machine(TTH_WIFI_CONNECT_TIMEOUT_MS, 1),
      _hasConfig(false),
      _driverStarted(false),
      _transition(false),
      _m1Reported(false),
      _attemptStartedMs(0),
      _connectedAtMs(0) {
  config::clear(_config);
}

void WifiLink::begin() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  _machine.reseed(esp_random());
}

void WifiLink::apply(const config::DeviceConfig* next, uint32_t nowMs) {
  config::clear(_config);
  _hasConfig = (next != nullptr);
  if (_hasConfig) {
    _config = *next;
    // Metadata only: never the SSID or password themselves.
    diag::log().printf("[wifi] configuration applied (ssid %u bytes, %s)",
                       static_cast<unsigned>(_config.ssidLength),
                       _config.passwordLength == 0 ? "open network"
                                                   : "password set");
  } else {
    diag::log().printf("[wifi] not provisioned: Wi-Fi off (provision over USB "
                       "serial - !prov help)");
  }
  execute(_machine.start(_hasConfig, nowMs), nowMs);
}

int WifiLink::rssi() const {
  if (!_driverStarted || WiFi.status() != WL_CONNECTED) return 0;
  return WiFi.RSSI();
}

bool WifiLink::consumeTransition() {
  const bool transition = _transition;
  _transition = false;
  return transition;
}

void WifiLink::poll(uint32_t nowMs) {
  const LinkState before = _machine.state();
  const bool connected = _driverStarted && WiFi.status() == WL_CONNECTED;
  const LinkAction action = _machine.poll(nowMs, connected);
  const LinkState after = _machine.state();

  if (before != after && after == LinkState::Connected) {
    _connectedAtMs = nowMs;
    const IPAddress ip = WiFi.localIP();
    diag::log().printf("[wifi] connected in %lu ms: rssi=%d dBm channel=%d "
                       "ip=%u.%u.%u.%u",
                       static_cast<unsigned long>(nowMs - _attemptStartedMs),
                       static_cast<int>(WiFi.RSSI()),
                       static_cast<int>(WiFi.channel()),
                       static_cast<unsigned>(ip[0]), static_cast<unsigned>(ip[1]),
                       static_cast<unsigned>(ip[2]), static_cast<unsigned>(ip[3]));
    if (!_m1Reported) {
      // M1 (PHASE6_PLAN §7): Wi-Fi associated, no TLS yet.
      _m1Reported = true;
      const diag::MemoryReport m = diag::readMemoryReport();
      diag::log().printf("[mem] M1 wifi connected: internal free=%lu largest=%lu "
                         "min=%lu | psram free=%lu largest=%lu",
                         static_cast<unsigned long>(m.internalFree),
                         static_cast<unsigned long>(m.internalLargestBlock),
                         static_cast<unsigned long>(m.internalMinFree),
                         static_cast<unsigned long>(m.psramFree),
                         static_cast<unsigned long>(m.psramLargestBlock));
    }
  } else if (before != after && after == LinkState::Backoff) {
    const wl_status_t status = _driverStarted ? WiFi.status() : WL_IDLE_STATUS;
    if (_machine.lastFailure() == LinkFailure::LinkLost) {
      diag::log().printf("[wifi] link lost after %lu s (%s) -> retry in %lu ms",
                         static_cast<unsigned long>((nowMs - _connectedAtMs) / 1000u),
                         wifiStatusName(status),
                         static_cast<unsigned long>(_machine.lastDelayMs()));
    } else {
      diag::log().printf("[wifi] no connection after %lu ms (%s) -> retry in "
                         "%lu ms (failures %lu)",
                         static_cast<unsigned long>(nowMs - _attemptStartedMs),
                         wifiStatusName(status),
                         static_cast<unsigned long>(_machine.lastDelayMs()),
                         static_cast<unsigned long>(_machine.attempt()));
    }
  }

  execute(action, nowMs);
}

bool WifiLink::startDriver() {
  if (_driverStarted) return true;
  _transition = true;
  TTH_TIME_BLOCK("wifi.driverStart");
  if (!WiFi.mode(WIFI_STA)) return false;
  _driverStarted = true;
  return true;
}

void WifiLink::stopDriver() {
  if (!_driverStarted) return;
  _transition = true;
  TTH_TIME_BLOCK("wifi.driverStop");
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  _driverStarted = false;
}

void WifiLink::execute(LinkAction action, uint32_t nowMs) {
  switch (action) {
    case LinkAction::None:
      return;

    case LinkAction::StartConnect: {
      if (!_hasConfig) return;
      if (!startDriver()) {
        // Stays Connecting; the connect timeout moves it to backoff.
        diag::log().printf("[wifi] *** Wi-Fi driver did not start ***");
        return;
      }
      _transition = true;
      _attemptStartedMs = nowMs;
      diag::log().printf("[wifi] connecting (attempt %lu)",
                         static_cast<unsigned long>(_machine.attempt() + 1u));
      TTH_TIME_BLOCK("wifi.begin");
      // Drop any half-finished attempt, then start this one. The strings are
      // NUL-terminated inside DeviceConfig; an empty password means an open
      // network.
      WiFi.disconnect(false, false);
      WiFi.begin(reinterpret_cast<const char*>(_config.ssid),
                 _config.passwordLength == 0
                     ? nullptr
                     : reinterpret_cast<const char*>(_config.password));
      return;
    }

    case LinkAction::Disconnect: {
      if (_machine.state() == LinkState::Unprovisioned) {
        stopDriver();
        return;
      }
      if (!_driverStarted) return;
      _transition = true;
      TTH_TIME_BLOCK("wifi.disconnect");
      WiFi.disconnect(false, false);
      return;
    }
  }
}

}  // namespace tth
