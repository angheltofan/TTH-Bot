#include "tth/PcmGain.h"

#include <math.h>

namespace tth {

int32_t dbToGainQ12(float db) {
  // `!(db > 0)` is also true for NaN.
  if (!(db > 0.0f)) return kGainUnityQ12;
  if (db > kMaxLoopbackGainDb) db = kMaxLoopbackGainDb;
  const float linear = powf(10.0f, db / 20.0f);
  int32_t q = static_cast<int32_t>(linear * static_cast<float>(kGainUnityQ12) +
                                   0.5f);
  if (q > kMaxGainQ12) q = kMaxGainQ12;
  return q;
}

float gainQ12ToDb(int32_t gainQ12) {
  if (gainQ12 <= 0) return -120.0f;
  return 20.0f * log10f(static_cast<float>(gainQ12) /
                        static_cast<float>(kGainUnityQ12));
}

int16_t saturateToInt16(int32_t value) {
  if (value > 32767) return 32767;
  if (value < -32768) return -32768;
  return static_cast<int16_t>(value);
}

int32_t amplify(int16_t sample, int32_t gainQ12) {
  if (gainQ12 < 0) gainQ12 = 0;
  if (gainQ12 > kMaxGainQ12) gainQ12 = kMaxGainQ12;
  // |sample| <= 32768 and gainQ12 <= 32536: |product| <= 1 066 139 648.
  const int32_t product = static_cast<int32_t>(sample) * gainQ12;
  // Symmetric rounding by division; no shift of a negative number.
  return (product >= 0) ? (product + kGainUnityQ12 / 2) / kGainUnityQ12
                        : -((-product + kGainUnityQ12 / 2) / kGainUnityQ12);
}

int16_t softLimit(int32_t value, bool* limited) {
  // amplify() bounds |value| to about 260 000, so negation cannot overflow.
  const int32_t magnitude = (value < 0) ? -value : value;
  if (magnitude <= kLimiterThreshold) {
    if (limited != nullptr) *limited = false;
    return saturateToInt16(value);
  }

  const int64_t over = static_cast<int64_t>(magnitude - kLimiterThreshold);
  const int64_t range = static_cast<int64_t>(kPcmFullScale - kLimiterThreshold);
  // Strictly less than `range` for any finite `over`: never reaches 32767.
  const int32_t shaped =
      kLimiterThreshold + static_cast<int32_t>((range * over) / (over + range));

  if (limited != nullptr) *limited = true;
  return saturateToInt16((value < 0) ? -shaped : shaped);
}

int16_t applyGain(int16_t sample, int32_t gainQ12, bool* limited) {
  if (gainQ12 == kGainUnityQ12) {
    // 0 dB is an exact bypass: the A/B reference is the recording itself.
    if (limited != nullptr) *limited = false;
    return sample;
  }
  return softLimit(amplify(sample, gainQ12), limited);
}

TurnLevelHistogram::TurnLevelHistogram() { reset(); }

void TurnLevelHistogram::reset() {
  for (uint32_t i = 0; i < kBins; ++i) _bins[i] = 0;
  _count = 0;
  _peak = 0;
}

void TurnLevelHistogram::add(const int16_t* samples, uint32_t count) {
  if (samples == nullptr) return;
  for (uint32_t i = 0; i < count; ++i) {
    const int32_t v = samples[i];
    const int32_t magnitude = (v < 0) ? -v : v;  // up to 32768
    uint32_t bin = static_cast<uint32_t>(magnitude) / kBinWidth;
    if (bin >= kBins) bin = kBins - 1;
    ++_bins[bin];
    if (magnitude > _peak) _peak = magnitude;
  }
  _count += count;
}

int32_t TurnLevelHistogram::levelCovering(uint32_t permille) const {
  if (_count == 0) return 0;
  if (permille > 1000) permille = 1000;
  const uint64_t needed =
      (static_cast<uint64_t>(_count) * permille + 999u) / 1000u;
  uint64_t cumulative = 0;
  for (uint32_t bin = 0; bin < kBins; ++bin) {
    cumulative += _bins[bin];
    if (cumulative >= needed) {
      const int32_t upperEdge = static_cast<int32_t>((bin + 1) * kBinWidth) - 1;
      return (upperEdge < _peak) ? upperEdge : _peak;
    }
  }
  return _peak;
}

int32_t fitTurnGainQ12(int32_t configuredGainQ12, int32_t robustPeak) {
  if (configuredGainQ12 <= kGainUnityQ12) return configuredGainQ12;
  if (robustPeak <= 0) return configuredGainQ12;

  int64_t fit = (static_cast<int64_t>(kLimiterThreshold) * kGainUnityQ12) /
                robustPeak;
  if (fit < kGainUnityQ12) fit = kGainUnityQ12;
  if (fit > configuredGainQ12) fit = configuredGainQ12;
  return static_cast<int32_t>(fit);
}

}  // namespace tth
