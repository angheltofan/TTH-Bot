#pragma once

#include <stdint.h>

// Digital gain for LOCAL LOOPBACK playback only.
//
// WHY LOOPBACK NEEDS ITS OWN GAIN
//
// The recording sits well below digital full scale -- a voice at arm's length
// into the SPM1423 peaks far under 32767 -- while synthetic speech is
// generated at about -5 dBFS and Gemini's speech arrives normalised. Raising
// the speaker's master volume would make ALL of those louder together (and
// M5Unified applies it SQUARED). A gain on the loopback path lifts only the
// recording, into headroom it is not using.
//
// Nothing here is used by synthetic audio or by any future network source:
// the only caller is LocalMockTurnSource's loopback branch.
//
// THE STRATEGY: ONE GAIN PER TURN, PLUS A SOFT KNEE
//
//  1. While the user turn is received, a histogram of |sample| is built.
//  2. When the response starts, ONE gain is chosen for the whole turn: the
//     configured gain, reduced only if it would push the 99.9th-percentile
//     level above the limiter knee. Never below unity.
//  3. Each sample is amplified with 32-bit arithmetic, then passed through a
//     memoryless soft-knee limiter above -3 dBFS that approaches, but never
//     reaches, full scale.
//
// Because the gain is constant for the turn and the limiter has no memory,
// there is no pumping between words, and the output is a pure function of
// each input sample -- identical however the turn is split into chunks.
// Nothing normalises chunk by chunk.
//
// 0 dB is an EXACT BYPASS: no amplification and no limiter, so the recording
// is played bit for bit. It is the A/B reference.

namespace tth {

// Gains are fixed point with 12 fractional bits: 4096 is unity.
const int32_t kGainUnityQ12 = 4096;

// The largest gain accepted. 18 dB is 7.94x, i.e. 32536 in Q12. Together with
// |sample| <= 32768 it bounds every product at 32768 * 32536 = 1 066 139 648,
// safely inside int32_t (2 147 483 647). amplify() clamps to this, so no
// caller can make the multiplication wrap.
const float kMaxLoopbackGainDb = 18.0f;
const int32_t kMaxGainQ12 = 32536;

// The soft limiter's knee: -3 dBFS. Below it the signal is untouched.
const int32_t kLimiterThreshold = 23198;
const int32_t kPcmFullScale = 32767;

// dB -> Q12. Clamped to [0, kMaxLoopbackGainDb]: loopback gain never
// attenuates. Negative or NaN input gives unity.
int32_t dbToGainQ12(float db);
float gainQ12ToDb(int32_t gainQ12);

int16_t saturateToInt16(int32_t value);

// sample * gain in signed 32-bit arithmetic, rounded half away from zero.
// Gain clamped to [0, kMaxGainQ12]. Cannot wrap.
int32_t amplify(int16_t sample, int32_t gainQ12);

// Memoryless soft knee above kLimiterThreshold:
//   y = T + R*d / (d + R),  d = |x| - T,  R = 32767 - T
// Continuous with slope 1 at the knee, strictly increasing, and bounded
// below full scale, so loud speech rounds off instead of clipping hard.
// `limited` (optional) reports whether the knee was entered.
int16_t softLimit(int32_t value, bool* limited);

// The whole per-sample path. Unity gain returns `sample` unchanged.
int16_t applyGain(int16_t sample, int32_t gainQ12, bool* limited);

// |sample| distribution for one turn, to choose that turn's gain.
class TurnLevelHistogram {
 public:
  static const uint32_t kBins = 128;
  static const uint32_t kBinWidth = 256;  // 128 * 256 = 32768

  TurnLevelHistogram();
  void reset();
  void add(const int16_t* samples, uint32_t count);

  uint32_t count() const { return _count; }
  int32_t peak() const { return _peak; }

  // The level at or below which `permille`/1000 of the samples lie (upper
  // edge of the bin, capped at the true peak). 0 when empty.
  int32_t levelCovering(uint32_t permille) const;

 private:
  uint32_t _bins[kBins];
  uint32_t _count;
  int32_t _peak;
};

// One gain for the whole turn: `configured`, reduced only as far as needed to
// keep `robustPeak` at the limiter knee, and never below unity. A unity (or
// lower) configured gain is returned unchanged.
int32_t fitTurnGainQ12(int32_t configuredGainQ12, int32_t robustPeak);

}  // namespace tth
