#include "tth/ConfigStore.h"

#include <string.h>

namespace tth {

const char* const ConfigStore::kSlotKeyA = "cfgA";
const char* const ConfigStore::kSlotKeyB = "cfgB";

const char* toString(CommitResult result) {
  switch (result) {
    case CommitResult::Ok:
      return "ok";
    case CommitResult::Incomplete:
      return "incomplete";
    case CommitResult::GenerationExhausted:
      return "generation counter exhausted - reset required";
    case CommitResult::WriteFailed:
      return "write failed";
    case CommitResult::VerifyFailed:
      return "read-back verification failed";
  }
  return "invalid";
}

ConfigStore::ConfigStore(IConfigStorage& storage)
    : _storage(storage), _hasConfig(false), _generation(0), _slot(0) {
  config::clear(_active);
  wipeBuffers();
}

void ConfigStore::wipeBuffers() {
  memset(_buffer, 0, sizeof(_buffer));
  memset(_verify, 0, sizeof(_verify));
}

SlotReport ConfigStore::readSlot(const char* key, config::DeviceConfig& out) {
  SlotReport report;
  report.generation = 0;
  config::clear(out);
  const size_t length = _storage.read(key, _buffer, sizeof(_buffer));
  if (length == 0) {
    report.status = config::RecordStatus::Absent;
  } else if (length > sizeof(_buffer)) {
    report.status = config::RecordStatus::BadLength;
  } else {
    report.status = config::decodeRecord(_buffer, length, out, report.generation);
  }
  wipeBuffers();
  return report;
}

LoadReport ConfigStore::load() {
  config::DeviceConfig a;
  config::DeviceConfig b;
  LoadReport report;
  report.a = readSlot(kSlotKeyA, a);
  report.b = readSlot(kSlotKeyB, b);

  config::clear(_active);
  _hasConfig = false;
  _generation = 0;
  _slot = 0;

  const bool aOk = report.a.status == config::RecordStatus::Ok;
  const bool bOk = report.b.status == config::RecordStatus::Ok;
  if (aOk && (!bOk || report.a.generation >= report.b.generation)) {
    _active = a;
    _generation = report.a.generation;
    _slot = 'A';
    _hasConfig = true;
  } else if (bOk) {
    _active = b;
    _generation = report.b.generation;
    _slot = 'B';
    _hasConfig = true;
  }
  config::clear(a);
  config::clear(b);

  report.chosen = _slot;
  report.generation = _generation;
  return report;
}

CommitResult ConfigStore::commit(const config::DeviceConfig& next,
                                 config::Field* problem, config::Check* why) {
  config::Field badField = config::Field::Ssid;
  const config::Check complete = config::checkComplete(next, &badField);
  if (complete != config::Check::Ok) {
    if (problem != nullptr) *problem = badField;
    if (why != nullptr) *why = complete;
    return CommitResult::Incomplete;
  }
  if (_hasConfig && _generation == 0xFFFFFFFFu) {
    return CommitResult::GenerationExhausted;
  }

  const uint32_t generation = _hasConfig ? _generation + 1u : 1u;
  // Never the slot holding the live configuration.
  const char targetSlot = (_slot == 'A') ? 'B' : 'A';
  const char* target = (targetSlot == 'A') ? kSlotKeyA : kSlotKeyB;

  const size_t length =
      config::encodeRecord(next, generation, _buffer, sizeof(_buffer));
  if (length == 0) {
    wipeBuffers();
    return CommitResult::Incomplete;
  }

  if (!_storage.write(target, _buffer, length)) {
    wipeBuffers();
    return CommitResult::WriteFailed;
  }

  // Read back exactly what is now in flash and decode it as a boot would.
  const size_t back = _storage.read(target, _verify, sizeof(_verify));
  bool verified = back == length && memcmp(_verify, _buffer, length) == 0;
  if (verified) {
    config::DeviceConfig decoded;
    uint32_t decodedGeneration = 0;
    verified = config::decodeRecord(_verify, length, decoded, decodedGeneration) ==
                   config::RecordStatus::Ok &&
               decodedGeneration == generation;
    config::clear(decoded);
  }
  wipeBuffers();
  if (!verified) return CommitResult::VerifyFailed;

  _active = next;
  _generation = generation;
  _slot = targetSlot;
  _hasConfig = true;
  return CommitResult::Ok;
}

bool ConfigStore::reset() {
  if (!_storage.eraseAll()) return false;
  config::clear(_active);
  _hasConfig = false;
  _generation = 0;
  _slot = 0;
  return true;
}

}  // namespace tth
