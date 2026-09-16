#include "tth/SpritePlacement.h"

namespace tth {

const char* toString(SpriteMemory memory) {
  switch (memory) {
    case SpriteMemory::Psram:
      return "PSRAM";
    case SpriteMemory::InternalDram:
      return "internal DRAM";
  }
  return "invalid";
}

bool spriteInRequestedRegion(SpriteMemory requested, uintptr_t bufferAddress) {
  if (bufferAddress == 0) return false;
  switch (requested) {
    case SpriteMemory::Psram:
      return isPsram(bufferAddress);
    case SpriteMemory::InternalDram:
      return isDmaCapable(bufferAddress);
  }
  return false;
}

}  // namespace tth
