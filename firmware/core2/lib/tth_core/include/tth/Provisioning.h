#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tth/ConfigStore.h"
#include "tth/DeviceConfig.h"
#include "tth/RandomSource.h"
#include "tth/TrustAnchor.h"

// USB-serial provisioning commands (Step 6.1; CA commands added in 6.2).
//
// Every command is one line starting with '!' (the '!' is stripped before it
// reaches handleLine):
//
//   !prov help                    list the commands
//   !prov ping                    liveness check for tools
//   !prov show                    stored (and staged) configuration, redacted
//   !prov begin                   start editing (a copy of the stored config)
//   !prov set <field> <value>     field: ssid pass url id token; value = the
//                                 rest of the line, exactly (may be empty for
//                                 pass = open network)
//   !prov sethex <field> <hex>    the same with the value hex-encoded
//   !prov commit                  validate everything and store atomically
//   !prov abort                   discard the staged changes
//   !prov reset                   erase TTH Bot settings; issues a code
//   !prov reset confirm <code>    ...confirmed within 30 s
//
//   !prov ca begin                start staging the gateway CA (PUBLIC)
//   !prov ca line <pem line>      append one PEM line
//   !prov ca commit               check (structure + TLS parser) and store
//   !prov ca abort                discard the staged CA
//   !prov ca clear                store "no CA": the gateway stays disabled
//
// GUARANTEES
//
//   * A value is validated before it is staged; a bad value changes nothing.
//   * The stored configuration changes ONLY on a successful commit or a
//     confirmed reset. A malformed command, an invalid value, an incomplete
//     commit or a failed write all leave it exactly as it was.
//   * No output ever contains the SSID, password or token — not in
//     confirmations, not in errors, not even when a typo puts a secret where
//     a command name was expected. Errors name the field and the rule only.
//   * Writes (commit, reset, ca commit, ca clear) are refused while a turn or
//     playback is active, so provisioning never lands flash I/O in the middle
//     of audio.
//   * Staging and decoding buffers are zeroed after use.

namespace tth {

class ILineSink {
 public:
  virtual ~ILineSink() {}
  virtual void line(const char* text) = 0;
};

// Parses a CA bundle exactly as the TLS stack will (mbedTLS on the device)
// and writes a short, non-secret description (certificate count, subject).
class ICertificateCheck {
 public:
  virtual ~ICertificateCheck() {}
  virtual bool check(const char* pem, size_t length, char* summary,
                     size_t capacity) = 0;
};

// A redacted report of `config` under `label` ("stored", "staged"). With
// `present` false, a single "none" line.
void reportConfig(const char* label, const config::DeviceConfig& config,
                  bool present, ILineSink& out);

// One line about the stored trust anchor. `check` may be null.
void reportTrustAnchor(const char* label, const TrustAnchorStore& store,
                       ICertificateCheck* check, ILineSink& out);

struct ProvisioningResult {
  bool configChanged;  // the stored configuration or CA was replaced or erased
  bool showRequested;
};

class ProvisioningSession {
 public:
  static const uint32_t kResetConfirmWindowMs = 30000;
  static const size_t kMaxLineBytes = 480;

  ProvisioningSession(ConfigStore& store, IRandomSource& random);

  // Enables the `ca` commands. `staging` holds kMaxPemBytes + 1 bytes. `check`
  // may be null (structural check only).
  void attachTrustAnchors(TrustAnchorStore& store, char* staging, size_t capacity,
                          ICertificateCheck* check);

  ProvisioningResult handleLine(const char* line, size_t length,
                                bool writesAllowed, uint32_t nowMs,
                                ILineSink& out);

  bool isStaging() const { return _staging; }
  bool isResetPending() const { return _resetPending; }
  bool isCaStaging() const { return _caActive; }

 private:
  void help(ILineSink& out);
  void show(ILineSink& out);
  void begin(ILineSink& out);
  void abort(ILineSink& out);
  void set(bool hex, const char* rest, size_t restLength, ILineSink& out);
  bool commit(bool writesAllowed, ILineSink& out);
  void requestReset(uint32_t nowMs, ILineSink& out);
  bool confirmReset(const char* code, size_t codeLength, bool writesAllowed,
                    uint32_t nowMs, ILineSink& out);
  void confirmStaged(config::Field field, ILineSink& out);
  bool handleCa(const char* rest, size_t restLength, bool writesAllowed,
                ILineSink& out);
  void discardCa();

  ConfigStore& _store;
  IRandomSource& _random;
  config::DeviceConfig _staged;
  bool _staging;
  bool _resetPending;
  uint16_t _resetCode;
  uint32_t _resetDeadlineMs;
  uint8_t _decoded[config::kUrlMaxBytes];

  TrustAnchorStore* _trust;
  char* _caStaging;
  size_t _caCapacity;
  size_t _caLength;
  uint16_t _caLines;
  bool _caActive;
  ICertificateCheck* _certCheck;
};

}  // namespace tth
