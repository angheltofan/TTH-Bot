#include "tth/AudioFormat.h"

namespace tth {

AudioFormat monoS16(uint32_t sampleRate) {
  AudioFormat format;
  format.sampleRate = sampleRate;
  format.channels = 1;
  format.encoding = SampleEncoding::S16LE;
  return format;
}

bool sameFormat(const AudioFormat& a, const AudioFormat& b) {
  return a.sampleRate == b.sampleRate && a.channels == b.channels &&
         a.encoding == b.encoding;
}

bool isPlayable(const AudioFormat& format) {
  return format.channels == 1 && format.encoding == SampleEncoding::S16LE &&
         format.sampleRate >= kMinPlayableRate &&
         format.sampleRate <= kMaxPlayableRate;
}

}  // namespace tth
