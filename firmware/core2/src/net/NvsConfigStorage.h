#pragma once

#include <Preferences.h>

#include "tth/ConfigStore.h"

namespace tth {

// ConfigStore's flash: the NVS namespace "tth" (PHASE6_PLAN §4.1).
//
// eraseAll() is nvs_erase_all on THIS namespace's handle, so a TTH Bot reset
// never touches unrelated NVS data (the Wi-Fi driver's calibration, other
// libraries' settings).
//
// Not encrypted at rest (D8): physical access to the flash exposes the Wi-Fi
// password and this robot's revocable device token.
class NvsConfigStorage : public IConfigStorage {
 public:
  NvsConfigStorage();

  // Opens the namespace read-write. Call once, before ConfigStore::load().
  bool begin();

  size_t read(const char* key, uint8_t* out, size_t capacity) override;
  bool write(const char* key, const uint8_t* data, size_t length) override;
  bool eraseAll() override;

  bool isOpen() const { return _open; }

 private:
  Preferences _prefs;
  bool _open;
};

}  // namespace tth
