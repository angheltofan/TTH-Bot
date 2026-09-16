#include "tth/PcmProcessing.h"

#include <math.h>

namespace tth {

namespace {

const float kFullScale = 32768.0f;

int16_t saturate(int32_t v) {
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return static_cast<int16_t>(v);
}

}  // namespace

ChunkLevels measureLevels(const int16_t* samples, uint32_t count) {
  ChunkLevels levels;
  levels.rms = 0.0f;
  levels.peak = 0.0f;
  if (samples == nullptr || count == 0) return levels;

  double sumSquares = 0.0;
  int32_t peakAbs = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const int32_t s = samples[i];
    sumSquares += static_cast<double>(s) * static_cast<double>(s);
    const int32_t magnitude = s < 0 ? -s : s;
    if (magnitude > peakAbs) peakAbs = magnitude;
  }

  levels.rms = static_cast<float>(sqrt(sumSquares / count) / kFullScale);
  levels.peak = static_cast<float>(peakAbs) / kFullScale;
  if (levels.rms > 1.0f) levels.rms = 1.0f;
  if (levels.peak > 1.0f) levels.peak = 1.0f;
  return levels;
}

ChunkLevels removeDcOffsetAndMeasure(int16_t* samples, uint32_t count) {
  if (samples == nullptr || count == 0) return measureLevels(samples, count);

  int64_t sum = 0;
  for (uint32_t i = 0; i < count; ++i) sum += samples[i];

  // Rounded rather than truncated, so a small offset is not consistently
  // under-corrected.
  const int32_t mean =
      static_cast<int32_t>((sum >= 0) ? ((sum + (count / 2)) / count)
                                      : ((sum - (static_cast<int64_t>(count) / 2)) /
                                         count));

  if (mean != 0) {
    for (uint32_t i = 0; i < count; ++i) {
      samples[i] = saturate(static_cast<int32_t>(samples[i]) - mean);
    }
  }

  return measureLevels(samples, count);
}

}  // namespace tth
