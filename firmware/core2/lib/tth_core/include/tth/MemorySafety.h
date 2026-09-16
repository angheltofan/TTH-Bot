#pragma once

#include <stdint.h>

// ESP32 memory-region checks for audio buffers.
//
// WHY THIS EXISTS -- the Phase 4 crash
//
// The ESP32 has three data regions, and they are NOT interchangeable:
//
//   DRAM   0x3FFAE000-0x3FFFFFFF   byte addressable, DMA capable
//   IRAM   0x40080000-0x4009FFFF   32-BIT ALIGNED WORD ACCESS ONLY
//   SPIRAM 0x3F800000-0x3FBFFFFF   byte addressable, NOT DMA capable
//
// heap_caps_malloc(size, MALLOC_CAP_INTERNAL) means only "not SPIRAM". It will
// happily return IRAM. Any 8- or 16-bit access to IRAM raises a LoadStoreError
// (EXCCAUSE 3) and panics the core.
//
// That is exactly what happened: a 1 KB chunk buffer allocated with
// MALLOC_CAP_INTERNAL landed at 0x40092464, and M5Unified's microphone task
// stores int16 samples straight into it (Mic_Class.cpp:743,
// `auto dst = (int16_t*)(current_rec->data); *dst++ = value;`). The first
// sample of the first turn killed the device.
//
// Every buffer holding int16 PCM must therefore be byte addressable, and any
// buffer an I2S DMA writes into must additionally be DRAM. These predicates
// are pure arithmetic so they are unit tested on the host, and the firmware
// checks every buffer against them at start-up rather than discovering the
// problem as a panic.

namespace tth {

const uintptr_t kEsp32DramLow = 0x3FFAE000u;
const uintptr_t kEsp32DramHigh = 0x40000000u;
const uintptr_t kEsp32IramLow = 0x40080000u;
const uintptr_t kEsp32IramHigh = 0x400A0000u;
const uintptr_t kEsp32SpiramLow = 0x3F800000u;
const uintptr_t kEsp32SpiramHigh = 0x3FC00000u;

// True when a 16-bit store to this address is legal. IRAM is not.
bool isByteAddressable(uintptr_t address);

// True when the address is in internal DRAM: byte addressable AND reachable
// by DMA. This is what a buffer handed to M5.Mic.record() must satisfy.
bool isDmaCapable(uintptr_t address);

// True when the address is in external PSRAM. Byte addressable, so it is fine
// for the turn buffer, but never valid as a DMA destination.
bool isPsram(uintptr_t address);

// Names the region, for diagnostics.
const char* memoryRegionName(uintptr_t address);

}  // namespace tth
