#include "net/NvsConfigStorage.h"

namespace tth {

namespace {
// Plan §4.1. At most 15 characters (NVS limit).
const char* const kNamespace = "tth";
}  // namespace

NvsConfigStorage::NvsConfigStorage() : _open(false) {}

bool NvsConfigStorage::begin() {
  _open = _prefs.begin(kNamespace, false);
  return _open;
}

size_t NvsConfigStorage::read(const char* key, uint8_t* out, size_t capacity) {
  // isKey() first: asking for the length of a missing key makes the
  // Preferences library log an error line on every boot.
  if (!_open || !_prefs.isKey(key)) return 0;
  const size_t length = _prefs.getBytesLength(key);
  if (length == 0 || length > capacity || out == nullptr) return length;
  return _prefs.getBytes(key, out, length);
}

bool NvsConfigStorage::write(const char* key, const uint8_t* data,
                             size_t length) {
  if (!_open || data == nullptr || length == 0) return false;
  return _prefs.putBytes(key, data, length) == length;
}

bool NvsConfigStorage::eraseAll() {
  if (!_open) return false;
  return _prefs.clear();
}

}  // namespace tth
