#pragma once

#include <stdint.h>

#include "tth/AudioFormat.h"
#include "tth/ITurnSource.h"
#include "tth/TurnBuffer.h"

// Sends the user's speech to the turn source WHILE it is being recorded.
//
// THE TURN BUFFER IS THE BACKPRESSURE RESERVOIR
//
// The streamer holds a read cursor into the same PSRAM TurnBuffer the capture
// fills. Nothing is copied into a second buffer, and nothing waits for the
// recording to finish:
//
//   M5.Mic -> CaptureController -> TurnBuffer.append() -> [commit]
//                                                            |
//                    TurnStreamer reads [cursor, committed) -+-> pushUserAudio()
//
// Busy leaves the cursor where it is; the samples are still in PSRAM and the
// same ones are offered again next loop. That is what makes backpressure
// lossless. The alternative -- pushing the capture's scratch chunk straight
// through -- cannot be: that chunk is overwritten by the next read, so Busy
// would lose it, or force the microphone to stop being re-queued, which loses
// audio in the hardware.
//
// INVARIANTS (see TurnBuffer.h)
//
//  * Reads only [cursor, committedSamples()) -- never an uncommitted sample.
//  * Holds a lease on the buffer from begin() until every committed sample has
//    been accepted, or the turn is abandoned. While it does, the buffer cannot
//    be reset, so the next recording cannot overwrite unsent audio.
//  * After the recording ends it keeps draining the tail; endUserTurn() is
//    sent only once the cursor reaches the final committed length.
//
// Bounded: one service() call pushes at most `maxFramesPerService` frames, so
// streaming can never itself become a long loop iteration.

namespace tth {

enum class StreamStatus : uint8_t {
  Idle = 0,
  // Holding a lease, sending as the audio is committed.
  Streaming,
  // Every sample accepted and endUserTurn() sent. Lease released.
  Finished,
  // The source returned Fatal. Lease released; the caller aborts the turn.
  Failed,
};

const char* toString(StreamStatus status);

struct StreamMetrics {
  uint32_t streamedSamples;
  uint32_t frames;
  // Busy results. Each one is a retry of the same samples, never a loss.
  uint32_t busyRetries;
  // Furthest the cursor fell behind the committed length.
  uint32_t maxLagSamples;
};

class TurnStreamer {
 public:
  // `frameSamples`: size of a pushed frame while the recording is live. 320
  // (20 ms at 16 kHz) matches the Flutter app, so the gateway sees identical
  // traffic from either embodiment.
  TurnStreamer(uint32_t frameSamples, uint32_t maxFramesPerService);

  // Starts streaming a turn whose audio is (or will be) in `buffer`. Takes the
  // lease and calls sink.beginUserTurn(). Returns false -- holding nothing --
  // if already streaming, the lease cannot be taken, or the sink refuses.
  bool begin(TurnBuffer& buffer, ITurnSource& sink, const AudioFormat& format);

  // `producerDone`: the capture has stopped appending for good. Until then
  // only whole frames are sent; the final partial frame goes once it is done.
  StreamStatus service(bool producerDone);

  // Drops the turn: releases the lease without calling endUserTurn(). The
  // caller cancels the source separately.
  void abandon();

  StreamStatus status() const { return _status; }
  bool isActive() const { return _status == StreamStatus::Streaming; }
  uint32_t cursor() const { return _cursor; }
  // How far the cursor is behind the committed audio right now.
  uint32_t lagSamples() const;
  const StreamMetrics& metrics() const { return _metrics; }

 private:
  void releaseLease();

  const uint32_t _frameSamples;
  const uint32_t _maxFramesPerService;

  TurnBuffer* _buffer;
  ITurnSource* _sink;
  AudioFormat _format;
  StreamStatus _status;
  uint32_t _cursor;
  StreamMetrics _metrics;
};

}  // namespace tth
