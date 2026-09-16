#pragma once

#include <stddef.h>
#include <stdint.h>

// Formatting for the end-of-response playback report.
//
// Two explicitly named lines, like the capture SUMMARY: with the loudness
// fields added, one line could approach the 224-byte log slot, and a lifecycle
// report is never allowed to truncate. The tests assert both lines fit at
// absurd worst-case values.

namespace tth {

struct PlaybackSummaryData {
  // Line 1: what happened to the stream.
  const char* end;
  uint32_t rate;
  uint32_t queued;
  uint32_t played;
  uint32_t samples;
  uint32_t underruns;
  uint32_t rejects;
  uint32_t refusals;
  const char* audioOwner;
  uint32_t drainMs;

  // Line 2: loudness.
  const char* source;    // "loopback" / "synthetic"
  float gainDb;          // PCM gain actually applied to this response
  float inputPeak;       // 0..1, before the gain (what the source had)
  float outputPeak;      // 0..1, what PcmPlayer received
  uint32_t limitedSamples;
  float maxLevel;        // highest chunk RMS the speaker played
  uint32_t masterVolume; // M5 speaker master volume this response played at
  float pathGainDb;      // M5 mixer attenuation at that volume
};

// Both return the length the complete line WOULD need, as snprintf does.
int formatPlaybackSummaryLine1(char* out, size_t capacity,
                               const PlaybackSummaryData& data);
int formatPlaybackSummaryLine2(char* out, size_t capacity,
                               const PlaybackSummaryData& data);

}  // namespace tth
