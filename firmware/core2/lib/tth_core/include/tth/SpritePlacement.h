#pragma once

#include <stdint.h>

#include "tth/MemorySafety.h"

// Where the face sprites live, decided at COMPILE TIME (PHASE6_PLAN §7, Step
// 6.2 memory gate).
//
// The renderer used to try internal DRAM and fall back to PSRAM when that
// allocation failed. That made the placement depend on whatever the heap
// looked like at boot, and hid the decision behind a runtime failure. Now the
// build states where each sprite goes, the renderer allocates exactly there,
// reads the address back, and refuses to render if it is anywhere else. There
// is no fallback in either direction.

namespace tth {

enum class SpriteMemory : uint8_t { Psram = 0, InternalDram };

const char* toString(SpriteMemory memory);

// True only when `bufferAddress` really lies in the requested region: PSRAM
// for Psram, DMA-capable internal DRAM for InternalDram. A null buffer is
// never in the right place.
bool spriteInRequestedRegion(SpriteMemory requested, uintptr_t bufferAddress);

}  // namespace tth
