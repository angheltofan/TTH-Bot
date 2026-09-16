#pragma once

#include <stdint.h>

#include "tth/FaceGeometry.h"
#include "tth/LogQueue.h"

namespace tth {

// Serial diagnostics printed once at boot, plus the periodic heartbeat.
//
// Phase 0 proved the hardware; this is the much smaller permanent version of
// that report, kept because the two things it watches -- PSRAM headroom and a
// loop that never stalls -- are exactly what the later phases can quietly
// break.
namespace diag {

// Board, chip, memory, and the build's compile-time configuration.
// `pttInputName` comes from the selected IPushToTalkInput, so the report can
// never disagree with the implementation actually compiled in.
// Returns false if something looks wrong enough to stop and investigate
// (wrong board, or less PSRAM than the design budget assumes).
bool printStartupReport(const char* pttInputName);

// One line of live health: uptime, free heap/PSRAM, and the worst main-loop
// iteration time seen since the previous heartbeat.
//
// `maxLoopMicros` is reset by the caller after each heartbeat so the figure is
// a per-interval worst case rather than an all-time one that stops moving.
// Heartbeat lines are QUEUED, not written -- writing them synchronously cost
// 21-31 ms each at 115200 baud. The caller emits them on separate loop
// iterations, one line per iteration.
void queueHeartbeat(LogQueue& queue, uint32_t uptimeMs,
                    uint32_t maxLoopSteadyMicros,
                    uint32_t maxLoopTransitionMicros,
                    uint32_t maxSerialWriteMicros, uint32_t loopCount,
                    const char* conversationState, const char* audioOwner,
                    const char* faceState, uint32_t logDrops,
                    uint32_t criticalLogDrops);

void queueCaptureLine(LogQueue& queue, uint32_t maxFacePushMicros,
                      uint32_t maxTransitionMicros, uint32_t captureBytes,
                      uint32_t sessionHighWaterBytes, uint32_t capacityBytes,
                      uint32_t maxReadMicros, uint32_t lastDrainTotalMs,
                      uint32_t maxDrainTotalMs, uint32_t maxDrainPollMicros);

// The resolved face layout, printed once at boot so a geometry regression is
// visible without a display.
void printFaceGeometry(const FaceGeometry& geometry);

// Where the memory actually is.
//
// Phase 4 puts the face sprites in internal DRAM for speed and the 1.4 MB turn
// buffer in PSRAM. Phase 5 adds Wi-Fi, TLS and WebSocket buffers, which also
// want internal heap -- so the sprite decision is provisional, and these are
// the figures that will settle it. `largestBlock` matters as much as `free`:
// TLS needs sizeable contiguous allocations, and a heap that is fragmented
// fails those while still reporting plenty free.
struct MemoryReport {
  uint32_t internalFree;
  uint32_t internalLargestBlock;
  uint32_t internalMinFree;  // low-water mark since boot
  uint32_t psramFree;
  uint32_t psramLargestBlock;
};

MemoryReport readMemoryReport();

// One-line memory status, emitted with every heartbeat.
void queueMemoryLine(LogQueue& queue, const MemoryReport& report,
                     const char* spriteRegion);

// Full memory picture, printed once at boot after every buffer is allocated.
void printMemoryReport(const MemoryReport& report, const char* eyesRegion,
                       const char* lowerFaceRegion, uint32_t spriteBytes,
                       uint32_t turnBufferBytes);

}  // namespace diag
}  // namespace tth
