#pragma once

#include <stdint.h>

// The explicit, immutable format every piece of PCM in the firmware carries.
//
// WHY THIS EXISTS
//
// The firmware handles audio at two rates that must never be crossed:
// microphone capture is 16 kHz and assistant speech is 24 kHz. Feeding 16 kHz
// samples to hardware configured for 24 kHz plays them 1.5x too fast -- and
// nothing crashes, it just sounds wrong, which is exactly the kind of mistake
// that survives review.
//
// So a rate is never implied by context. A playback stream declares its format
// once when it is opened, every chunk carries its own format, and a chunk that
// does not match the open stream is REJECTED. Nothing resamples, coerces or
// "plays it anyway".
//
// Portable: deliberately free of Config.h. The device layer builds the two
// concrete formats from its configuration and hands them in.

namespace tth {

enum class SampleEncoding : uint8_t {
  // Signed 16-bit, little-endian. The only encoding the firmware supports, and
  // the one both the Flutter app and Gemini Live use.
  S16LE = 0,
};

struct AudioFormat {
  uint32_t sampleRate;
  uint8_t channels;
  SampleEncoding encoding;
};

// Mono s16le at `sampleRate`. The only shape of format the firmware creates.
AudioFormat monoS16(uint32_t sampleRate);

// Exact equality. Two formats are compatible only if every field matches;
// "close enough" is how a 16 kHz buffer ends up on a 24 kHz stream.
bool sameFormat(const AudioFormat& a, const AudioFormat& b);

// Rates the speaker path accepts. Wide enough for any real speech format, and
// narrow enough that a zero or garbage rate is refused rather than played.
const uint32_t kMinPlayableRate = 8000;
const uint32_t kMaxPlayableRate = 48000;

// Mono, s16le, and a sane rate. Anything else is refused at openStream().
bool isPlayable(const AudioFormat& format);

// A borrowed VIEW of PCM samples plus the format they are in.
//
// OWNERSHIP: `samples` is never owned by the chunk. Whoever hands a chunk out
// states how long the pointer stays valid (see ITurnSource and TurnStreamer);
// a consumer that needs the data for longer must copy it before then. Nothing
// in the firmware frees memory through a chunk.
struct AudioChunk {
  const int16_t* samples;
  uint32_t count;  // samples, not bytes
  AudioFormat format;
};

}  // namespace tth
