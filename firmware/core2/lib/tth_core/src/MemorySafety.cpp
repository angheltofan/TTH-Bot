#include "tth/MemorySafety.h"

namespace tth {

namespace {

bool within(uintptr_t address, uintptr_t low, uintptr_t high) {
  return address >= low && address < high;
}

}  // namespace

bool isByteAddressable(uintptr_t address) {
  if (address == 0) return false;
  // IRAM is the trap: it is "internal" memory, but only word accessible.
  if (within(address, kEsp32IramLow, kEsp32IramHigh)) return false;
  return within(address, kEsp32DramLow, kEsp32DramHigh) ||
         within(address, kEsp32SpiramLow, kEsp32SpiramHigh);
}

bool isDmaCapable(uintptr_t address) {
  if (address == 0) return false;
  return within(address, kEsp32DramLow, kEsp32DramHigh);
}

bool isPsram(uintptr_t address) {
  if (address == 0) return false;
  return within(address, kEsp32SpiramLow, kEsp32SpiramHigh);
}

const char* memoryRegionName(uintptr_t address) {
  if (address == 0) return "null";
  if (within(address, kEsp32DramLow, kEsp32DramHigh)) return "DRAM";
  if (within(address, kEsp32IramLow, kEsp32IramHigh)) return "IRAM";
  if (within(address, kEsp32SpiramLow, kEsp32SpiramHigh)) return "PSRAM";
  return "unknown";
}

}  // namespace tth
