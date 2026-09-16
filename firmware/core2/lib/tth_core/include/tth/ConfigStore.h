#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tth/DeviceConfig.h"

// Transactional persistence of the DeviceConfig (Step 6.1).
//
// TWO SLOTS, A AND B. Each holds a complete record with a generation number
// and a CRC. A commit:
//   1. validates the whole new configuration (nothing is written otherwise);
//   2. writes it to the slot that does NOT hold the live configuration;
//   3. reads it back and decodes it (checksum, every field re-validated);
//   4. only then makes it the live configuration.
// Loading picks the valid slot with the highest generation.
//
// So an interrupted, failed or torn write can never destroy the last valid
// configuration: the slot holding it is never touched by a commit, and a
// damaged newer slot simply loses to it on the next boot.
//
// reset() erases the store's own namespace and nothing else.
//
// Portable: the flash sits behind IConfigStorage.

namespace tth {

class IConfigStorage {
 public:
  virtual ~IConfigStorage() {}
  // Returns the stored length of `key` (0 if absent). Copies the value into
  // `out` only when it fits `capacity`.
  virtual size_t read(const char* key, uint8_t* out, size_t capacity) = 0;
  // Stores `length` bytes under `key`. False if it could not be written.
  virtual bool write(const char* key, const uint8_t* data, size_t length) = 0;
  // Erases every key of THIS store's namespace only.
  virtual bool eraseAll() = 0;
};

enum class CommitResult : uint8_t {
  Ok = 0,
  Incomplete,
  GenerationExhausted,
  WriteFailed,
  VerifyFailed,
};

const char* toString(CommitResult result);

struct SlotReport {
  config::RecordStatus status;
  uint32_t generation;
};

struct LoadReport {
  SlotReport a;
  SlotReport b;
  char chosen;  // 'A', 'B', or 0 when there is no valid configuration
  uint32_t generation;
};

class ConfigStore {
 public:
  static const char* const kSlotKeyA;
  static const char* const kSlotKeyB;

  explicit ConfigStore(IConfigStorage& storage);

  LoadReport load();

  bool hasConfig() const { return _hasConfig; }
  const config::DeviceConfig& active() const { return _active; }
  uint32_t generation() const { return _generation; }
  char activeSlot() const { return _slot; }

  // On anything but Ok the live configuration is unchanged. For Incomplete,
  // `problem` and `why` say which field and what is wrong (either may be null).
  CommitResult commit(const config::DeviceConfig& next, config::Field* problem,
                      config::Check* why);

  bool reset();

 private:
  SlotReport readSlot(const char* key, config::DeviceConfig& out);
  void wipeBuffers();

  IConfigStorage& _storage;
  config::DeviceConfig _active;
  bool _hasConfig;
  uint32_t _generation;
  char _slot;
  uint8_t _buffer[config::kRecordMaxBytes];
  uint8_t _verify[config::kRecordMaxBytes];
};

}  // namespace tth
