#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tth/ConfigStore.h"
#include "tth/DeviceConfig.h"

// The gateway's trust anchor: the CA certificate(s) the device pins
// (PHASE6_PLAN §4.2; the `ca_pem` deferred from Step 6.1 to 6.2).
//
// A CA certificate is PUBLIC. It is provisioned over USB serial and stored in
// NVS so that a LAN development CA -- or a rotated production root -- can be
// installed without a firmware rebuild. Without a trust anchor the device does
// not connect to the gateway at all: there is no "insecure" mode.
//
// Stored like the device configuration: two A/B slots ("caA", "caB") holding
// complete records -- "TTHA" | version 1 | generation (u32 LE) | length (u16 LE)
// | PEM | CRC-32 -- written to the non-live slot, read back and re-validated
// before switching. An empty record means "no trust anchor" (a clear is a
// commit, so it is transactional too).
//
// The structural PEM check here is portable; the device additionally parses
// the bundle with mbedTLS before committing (ICertificateCheck).

namespace tth {
namespace trust {

const size_t kMaxPemBytes = 4096;
const uint8_t kMaxCertificates = 3;
const size_t kMaxPemLineBytes = 100;

enum class PemCheck : uint8_t {
  Ok = 0,
  Empty,
  TooLong,
  BadCharacter,
  NotCertificate,       // text or a PEM block other than CERTIFICATE
  PrivateKey,           // refused explicitly: a key never goes onto the robot
  TooManyCertificates,
  BadBase64,
  Unterminated,
  NoCertificate,
};

const char* toString(PemCheck check);

// Only CERTIFICATE blocks (1..kMaxCertificates) of base64 lines; blank lines
// between blocks and CRLF line ends are accepted.
PemCheck checkPemBundle(const char* pem, size_t length, uint8_t* certificates);

}  // namespace trust

struct TrustLoadReport {
  config::RecordStatus a;
  uint32_t generationA;
  config::RecordStatus b;
  uint32_t generationB;
  char chosen;  // 'A', 'B' or 0
  uint32_t generation;
};

class TrustAnchorStore {
 public:
  static const char* const kSlotKeyA;
  static const char* const kSlotKeyB;
  // "TTHA" + version + generation + length + CRC.
  static const size_t kRecordOverhead = 15;
  static const size_t kScratchBytes = trust::kMaxPemBytes + kRecordOverhead;

  explicit TrustAnchorStore(IConfigStorage& storage);

  // `active`: kMaxPemBytes + 1 bytes; `scratchA` and `scratchB`: kScratchBytes
  // each. Supplied by the owner so they can live in PSRAM on the device.
  void attachBuffers(char* active, uint8_t* scratchA, uint8_t* scratchB);
  bool hasBuffers() const;

  TrustLoadReport load();

  bool hasAnchor() const { return _length > 0; }
  // NUL-terminated; "" without an anchor.
  const char* pem() const;
  size_t length() const { return _length; }
  uint8_t certificates() const { return _certificates; }
  uint32_t generation() const { return _generation; }
  char activeSlot() const { return _slot; }

  // On anything but Ok the live anchor is unchanged. Incomplete: `why` says
  // what is wrong with the PEM.
  CommitResult commit(const char* pem, size_t length, trust::PemCheck* why);

  // Commits "no trust anchor".
  CommitResult clear();

  // Drops the in-memory copy after the namespace was erased.
  void forget();

 private:
  CommitResult write(const char* pem, size_t length, uint8_t certificates);
  config::RecordStatus decode(const uint8_t* data, size_t length,
                              uint32_t& generation, bool copy);
  config::RecordStatus readSlot(const char* key, uint32_t& generation, bool copy);

  IConfigStorage& _storage;
  char* _active;
  uint8_t* _scratchA;
  uint8_t* _scratchB;
  bool _hasRecord;
  size_t _length;
  uint8_t _certificates;
  uint32_t _generation;
  char _slot;
};

}  // namespace tth
