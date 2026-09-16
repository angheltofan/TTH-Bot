#include "tth/PlaybackSummary.h"

#include <stdio.h>

namespace tth {

int formatPlaybackSummaryLine1(char* out, size_t capacity,
                               const PlaybackSummaryData& data) {
  // `audio=` is read after release(), so it is the proof the speaker was
  // handed back; it must never be the field that gets cut.
  return snprintf(out, capacity,
                  "[play] SUMMARY1 end=%s rate=%luHz queued=%lu played=%lu "
                  "samples=%lu underruns=%lu rejects=%lu refusals=%lu "
                  "audio=%s drain=%lums",
                  data.end, static_cast<unsigned long>(data.rate),
                  static_cast<unsigned long>(data.queued),
                  static_cast<unsigned long>(data.played),
                  static_cast<unsigned long>(data.samples),
                  static_cast<unsigned long>(data.underruns),
                  static_cast<unsigned long>(data.rejects),
                  static_cast<unsigned long>(data.refusals), data.audioOwner,
                  static_cast<unsigned long>(data.drainMs));
}

int formatPlaybackSummaryLine2(char* out, size_t capacity,
                               const PlaybackSummaryData& data) {
  // PCM loudness first, then the physical speaker level it was played at.
  return snprintf(out, capacity,
                  "[play] SUMMARY2 source=%s gainDb=%+.1f inputPeak=%.3f "
                  "outputPeak=%.3f limitedSamples=%lu maxLevel=%.3f "
                  "master=%lu pathGainDb=%+.1f",
                  data.source, static_cast<double>(data.gainDb),
                  static_cast<double>(data.inputPeak),
                  static_cast<double>(data.outputPeak),
                  static_cast<unsigned long>(data.limitedSamples),
                  static_cast<double>(data.maxLevel),
                  static_cast<unsigned long>(data.masterVolume),
                  static_cast<double>(data.pathGainDb));
}

}  // namespace tth
