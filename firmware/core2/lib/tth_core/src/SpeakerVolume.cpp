#include "tth/SpeakerVolume.h"

#include <math.h>

namespace tth {

uint8_t clampSpeakerVolume(uint32_t volume) {
  return static_cast<uint8_t>(
      (volume > kSpeakerVolumeMax) ? kSpeakerVolumeMax : volume);
}

double speakerPathGain(uint32_t magnification, uint32_t master,
                       uint32_t channel) {
  // In double: 16 * 255^4 does not fit in 32 bits.
  const double m = static_cast<double>(master);
  const double c = static_cast<double>(channel);
  return static_cast<double>(magnification) * m * m * c * c / 68719476736.0;
}

double speakerPathGainDb(uint32_t magnification, uint32_t master,
                         uint32_t channel) {
  const double gain = speakerPathGain(magnification, master, channel);
  return (gain > 0.0) ? 20.0 * log10(gain) : -120.0;
}

}  // namespace tth
