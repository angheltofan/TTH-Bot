#pragma once

#include <stdint.h>

// The speaker master volume: its range, and what a given volume does to the
// signal in M5Unified's mixer.
//
// The firmware runs at the MAXIMUM M5Unified supports, 255. The loopback gain
// already brings the recording to ~0.9 of full scale; the master volume --
// applied SQUARED by M5Unified -- was the remaining attenuation, and at 255 it
// is essentially gone (x0.984, -0.1 dB). It is the physical speaker level, so
// it applies to loopback and synthetic playback alike.
//
// Portable, so the arithmetic is tested on the host.

namespace tth {

// M5Unified's master volume is 0..255.
const uint8_t kSpeakerVolumeMax = 255;

// min(volume, kSpeakerVolumeMax).
uint8_t clampSpeakerVolume(uint32_t volume);

// M5Unified's mixer amplitude factor for a 16-bit mono stream
// (Speaker_Class.cpp): magnification * master^2 * channel^2 / 2^36, before
// its hard int16 clamp. 1.0 would be full scale in, full scale out.
double speakerPathGain(uint32_t magnification, uint32_t master,
                       uint32_t channel);

// The same factor in dB (negative = attenuation). -120 for zero.
double speakerPathGainDb(uint32_t magnification, uint32_t master,
                         uint32_t channel);

}  // namespace tth
