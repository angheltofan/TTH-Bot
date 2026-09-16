#pragma once

#include <stdint.h>

// PCM chunk processing, applied to every captured chunk before it is stored.
//
// Phase 4 does exactly two things and no more: it removes the DC offset, and
// it measures the result. No AGC, no noise suppression, no resampling, no
// encoding and no voice-activity detection -- the bytes stored are the bytes
// that will later be transmitted, so anything added here would silently change
// what the model hears.
//
// WHY DC REMOVAL
//
// The Core2's SPM1423 PDM microphone has a standing offset: a silent room does
// not read as zero. Left in, that offset inflates every RMS measurement, biases
// the level meter, and wastes headroom. Removing it per chunk is cheap and
// keeps the stored audio centred.

namespace tth {

// Normalised loudness of one chunk, 0..1. Mirrors computePcmLevels() in the
// Flutter app (lib/features/voice/pcm_levels.dart) so figures logged here are
// directly comparable with the phone's.
struct ChunkLevels {
  float rms;
  float peak;
};

// Removes the mean from `samples` IN PLACE, then measures what remains.
//
// Measuring after the correction is deliberate: the levels reported must
// describe the audio actually stored, not the raw reading. Values are
// saturated to the int16 range, so a large offset cannot wrap a sample around.
ChunkLevels removeDcOffsetAndMeasure(int16_t* samples, uint32_t count);

// Measures without modifying. Used by tests and diagnostics.
ChunkLevels measureLevels(const int16_t* samples, uint32_t count);

}  // namespace tth
