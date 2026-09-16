#include "tth/CaptureSummary.h"

#include <stdio.h>

namespace tth {

int formatCaptureSummaryLine1(char* out, size_t capacity,
                              const CaptureSummaryData& data) {
  return snprintf(out, capacity,
                  "[capture] SUMMARY1 reason=%s duration=%lums samples=%lu "
                  "bytes=%lu highWater=%luB",
                  data.reason, static_cast<unsigned long>(data.durationMs),
                  static_cast<unsigned long>(data.samples),
                  static_cast<unsigned long>(data.bytes),
                  static_cast<unsigned long>(data.highWaterBytes));
}

int formatCaptureSummaryLine2(char* out, size_t capacity,
                              const CaptureSummaryData& data) {
  // `audio=` and `drainTotal=` are the proof that the microphone was released
  // before this was written, so they must never be the fields that get cut.
  return snprintf(out, capacity,
                  "[capture] SUMMARY2 rms min/avg/max=%.4f/%.4f/%.4f peak=%.4f "
                  "chunks=%lu failed=%lu dropped=%lu audio=%s drainTotal=%lums",
                  static_cast<double>(data.minRms),
                  static_cast<double>(data.averageRms),
                  static_cast<double>(data.maxRms),
                  static_cast<double>(data.peak),
                  static_cast<unsigned long>(data.chunks),
                  static_cast<unsigned long>(data.failedReads),
                  static_cast<unsigned long>(data.droppedSamples),
                  data.audioOwner,
                  static_cast<unsigned long>(data.drainTotalMs));
}

}  // namespace tth
