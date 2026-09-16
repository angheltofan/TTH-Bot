#pragma once

#include <stdint.h>

// The microphone, as CaptureController sees it.
//
// Everything M5Unified-specific lives behind this interface, which is what
// keeps the capture state machine portable and testable on the host.
//
// STOPPING IS THREE STEPS, NOT ONE
//
// The microphone runs on its own task, writing straight into our buffers. It
// cannot be torn down until that task has finished with them -- but waiting
// for it synchronously blocks the cooperative loop, which is exactly what
// pushed a loop iteration to 90 ms. So stopping is split:
//
//   requestStop()  stop queueing new reads. Returns immediately.
//   isDrained()    has the task finished with both buffers? Polled each loop.
//   finishStop()   final bookkeeping, once drained. Returns immediately.
//
// Only after isDrained() is true may the caller release the microphone through
// AudioBus. None of the three may block.

namespace tth {

class IAudioCapture {
 public:
  virtual ~IAudioCapture() {}

  // Begins recording. Called only after AudioBus has successfully acquired the
  // microphone. Returns false if the capture could not be started.
  virtual bool startCapture() = 0;

  // Non-blocking. Returns:
  //   > 0  the number of samples written to `dest`
  //     0  nothing ready yet -- come back next loop
  //   < 0  a read failure
  virtual int readChunk(int16_t* dest, uint32_t maxSamples) = 0;

  // Stops queueing new reads. MUST NOT block or wait for the hardware.
  virtual void requestStop() = 0;

  // True once the hardware is provably no longer writing our buffers, so the
  // microphone can safely be released. MUST NOT block.
  virtual bool isDrained() const = 0;

  // Final bookkeeping after draining. MUST NOT block.
  virtual void finishStop() = 0;

  // Longest single readChunk() call since the counter was last reset, in
  // microseconds.
  virtual uint32_t maxReadMicros() const = 0;
  virtual void resetReadStats() = 0;
};

}  // namespace tth
