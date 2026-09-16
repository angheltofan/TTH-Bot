#pragma once

#include <M5Unified.h>
#include <stdint.h>

#include "tth/IAudioCapture.h"

namespace tth {

// The M5Unified side of microphone capture.
//
// HOW M5Unified ACTUALLY WORKS (verified against 0.2.21 source)
//
//  * M5.Mic.record() hands a pointer to a background task, which writes int16
//    samples DIRECTLY into that buffer (Mic_Class.cpp:742-744). The buffer
//    must therefore stay valid and untouched until the recording completes,
//    and must be byte addressable DRAM -- see tth/MemorySafety.h for the
//    LoadStoreError this caused when it was not.
//
//  * isRecording() is NOT a bool. It returns the number of recordings still
//    queued: 0, 1 or 2 (Mic_Class.hpp:118). There are two queue slots
//    (_rec_info[2]), so up to two requests can be outstanding.
//
//  * record() returning true means "queued", NOT "finished". A chunk is ready
//    only when the outstanding count has DROPPED.
//
// WHY DOUBLE BUFFERING
//
// With a single buffer the microphone has nowhere to write between one
// recording completing and the next being queued, so every gap loses samples.
// Two buffers keep one recording always in flight while the other is being
// drained, which is what the queue depth of two exists for.
//
// This class never calls M5.Mic.begin() or M5.Mic.end(): AudioBus, via
// M5AudioDevice, is the only owner of those.
class M5MicrophoneCapture : public IAudioCapture {
 public:
  M5MicrophoneCapture();

  // Hands over TWO DMA-capable chunk buffers of `chunkSamples` each. Both must
  // be in internal DRAM; the caller checks that and refuses to start
  // otherwise. Nothing is allocated here or during a turn.
  void begin(int16_t* bufferA, int16_t* bufferB, uint32_t chunkSamples,
             uint32_t sampleRate);

  bool startCapture() override;
  int readChunk(int16_t* dest, uint32_t maxSamples) override;
  void requestStop() override;
  bool isDrained() const override;
  void finishStop() override;

  uint32_t maxReadMicros() const override { return _maxReadMicros; }
  void resetReadStats() override { _maxReadMicros = 0; }

  // Diagnostics: how many chunks were handed over, and how many times both
  // buffers were sitting full because the loop did not drain them fast enough.
  uint32_t chunksDelivered() const { return _chunksDelivered; }
  uint32_t queueStalls() const { return _queueStalls; }

 private:
  enum class State : uint8_t { Idle, Recording, Stopping };

  // Queues `_buffers[_writeSlot]` if there is room in M5Unified's two-slot
  // queue. Returns false only if record() was refused.
  bool topUpQueue();

  State _state;
  int16_t* _buffers[2];
  uint32_t _chunkSamples;
  uint32_t _sampleRate;

  // Which buffer is queued next, and which holds the oldest completed chunk.
  // They alternate in lockstep, so the slot freed by a drain is always the one
  // queued next.
  uint8_t _writeSlot;
  uint8_t _readSlot;

  // Queued but not yet known to have completed.
  uint8_t _inFlight;
  // Completed and holding data that has not been handed over yet.
  uint8_t _ready;

  uint32_t _maxReadMicros;
  uint32_t _chunksDelivered;
  uint32_t _queueStalls;
};

}  // namespace tth
