#pragma once

#include <stdint.h>
#include <stddef.h>

// Formatting for the end-of-turn report.
//
// WHY THIS IS PORTABLE CODE
//
// The single-line summary was ~218 characters at its worst, against a 192-byte
// queue slot. It would have been silently truncated -- memory-safe, but
// diagnostically worthless, and the fields most likely to be cut (dropped,
// highWater, audio, drainTotal) are exactly the ones that prove the turn
// completed correctly.
//
// So the report is split into two explicitly named lines, and the formatting
// lives here where a test can feed it worst-case values and assert that every
// field survives. A lifecycle report is never allowed to truncate; ordinary
// diagnostic chatter still may.

namespace tth {

struct CaptureSummaryData {
  const char* reason;
  uint32_t durationMs;
  uint32_t samples;
  uint32_t bytes;
  uint32_t highWaterBytes;

  float minRms;
  float averageRms;
  float maxRms;
  float peak;

  uint32_t chunks;
  uint32_t failedReads;
  uint32_t droppedSamples;

  const char* audioOwner;
  uint32_t drainTotalMs;
};

// Both return the length the complete line WOULD need, the way snprintf does.
// A value >= `capacity` means it was truncated, which for these two lines is a
// defect rather than a tolerable outcome -- the tests assert it cannot happen.
//
// Line 1 carries what was captured; line 2 carries the levels and the health
// of the turn, including the proof that the hardware was released.
int formatCaptureSummaryLine1(char* out, size_t capacity,
                              const CaptureSummaryData& data);
int formatCaptureSummaryLine2(char* out, size_t capacity,
                              const CaptureSummaryData& data);

}  // namespace tth
