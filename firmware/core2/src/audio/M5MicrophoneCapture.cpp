#include "audio/M5MicrophoneCapture.h"

#include <Arduino.h>

#include "diag/BlockTimer.h"
#include "diag/SerialLog.h"
#include "tth/MemorySafety.h"

namespace tth {

namespace {

// M5Unified's recording queue is two slots deep (Mic_Class.hpp: _rec_info[2]).
const uint8_t kQueueDepth = 2;

// stopCapture() waits this long for the microphone task to finish writing
// before handing the peripheral back. Bounded so a wedged task cannot hang the
// loop; M5.Mic.end() also waits for the task, so this is belt and braces.
const uint32_t kDrainTimeoutMs = 60;

}  // namespace

M5MicrophoneCapture::M5MicrophoneCapture()
    : _state(State::Idle),
      _chunkSamples(0),
      _sampleRate(16000),
      _writeSlot(0),
      _readSlot(0),
      _inFlight(0),
      _ready(0),
      _maxReadMicros(0),
      _chunksDelivered(0),
      _queueStalls(0) {
  _buffers[0] = nullptr;
  _buffers[1] = nullptr;
}

void M5MicrophoneCapture::begin(int16_t* bufferA, int16_t* bufferB,
                                uint32_t chunkSamples, uint32_t sampleRate) {
  _buffers[0] = bufferA;
  _buffers[1] = bufferB;
  _chunkSamples = (bufferA == nullptr || bufferB == nullptr) ? 0 : chunkSamples;
  _sampleRate = sampleRate;
}

bool M5MicrophoneCapture::topUpQueue() {
  // Never queue more than M5Unified can hold. Its record() would reject the
  // third request anyway, but asking is how the invariant stays visible.
  while ((_inFlight + _ready) < kQueueDepth) {
    int16_t* buffer = _buffers[_writeSlot];

    // The FIRST record() of a turn returns at once; the SECOND blocks for one
    // whole chunk. M5Unified advances its slot flip only when the task
    // FINISHES a slot (Mic_Class.cpp:628), so a second request cannot be
    // accepted until the first frame has been captured. Timed separately so
    // that is visible in the log rather than inferred.
    const char* label = (_inFlight == 0) ? "mic.record#1" : "mic.record#2";
    bool queued = false;
    {
      ::tth::diag::BlockTimer timer(label);
      queued = M5.Mic.record(buffer, _chunkSamples, _sampleRate);
    }

    if (!queued) {
      diag::log().printf("[mic] record() refused (inFlight=%u ready=%u)",
                         static_cast<unsigned>(_inFlight),
                         static_cast<unsigned>(_ready));
      return false;
    }
    _writeSlot ^= 1u;
    ++_inFlight;
  }
  return true;
}

bool M5MicrophoneCapture::startCapture() {
  if (_buffers[0] == nullptr || _buffers[1] == nullptr || _chunkSamples == 0) {
    diag::log().printf("[mic] no chunk buffers attached");
    return false;
  }

  // The microphone task writes int16 samples straight into these buffers, so
  // they must be byte-addressable DRAM. Checked here rather than discovered as
  // a LoadStoreError panic on the first sample.
  for (int i = 0; i < 2; ++i) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(_buffers[i]);
    if (!isDmaCapable(address)) {
      diag::log().printf(
          "[mic] chunk buffer %d at 0x%08lx is %s, not DMA-capable DRAM\r\n", i,
          static_cast<unsigned long>(address), memoryRegionName(address));
      return false;
    }
  }

  if (!M5.Mic.isEnabled()) {
    // AudioBus should have installed the microphone before we got here.
    diag::log().printf("[mic] microphone is not enabled");
    return false;
  }

  _state = State::Recording;
  _writeSlot = 0;
  _readSlot = 0;
  _inFlight = 0;
  _ready = 0;
  _chunksDelivered = 0;
  _queueStalls = 0;

  // Fill both queue slots so the microphone always has somewhere to write.
  if (!topUpQueue()) {
    diag::log().printf("[mic] could not prime the recording queue");
    _state = State::Idle;
    _inFlight = 0;
    _ready = 0;
    return false;
  }

  diag::log().printf("[mic] recording: %lu samples/chunk at %lu Hz, %u queued\r\n",
                static_cast<unsigned long>(_chunkSamples),
                static_cast<unsigned long>(_sampleRate),
                static_cast<unsigned>(_inFlight));
  return true;
}

int M5MicrophoneCapture::readChunk(int16_t* dest, uint32_t maxSamples) {
  if (_state != State::Recording) return 0;
  if (dest == nullptr || maxSamples == 0) return -1;

  const uint32_t start = micros();

  // isRecording() is a COUNT of outstanding requests, not a flag. A request
  // that has left the queue has completed, and its buffer now holds data.
  const uint32_t outstanding = M5.Mic.isRecording();
  if (outstanding > _inFlight) {
    // Should be impossible; treat a broken invariant as a read failure rather
    // than reading a buffer that may still be written.
    diag::log().printf("[mic] queue invariant broken: outstanding=%lu inFlight=%u\r\n",
                  static_cast<unsigned long>(outstanding),
                  static_cast<unsigned>(_inFlight));
    return -1;
  }

  const uint8_t completed =
      static_cast<uint8_t>(_inFlight - static_cast<uint8_t>(outstanding));
  if (completed > 0) {
    _ready = static_cast<uint8_t>(_ready + completed);
    _inFlight = static_cast<uint8_t>(outstanding);
    if (_ready >= kQueueDepth) {
      // Both buffers finished before we drained either: the loop was too slow.
      // No audio is lost -- neither buffer is re-queued until drained -- but it
      // is worth knowing about.
      ++_queueStalls;
    }
  }

  if (_ready == 0) {
    // Nothing finished yet. "Queued" is not "ready"; returning 0 here is what
    // stops a half-written buffer being read.
    if (!topUpQueue()) {
      const uint32_t elapsed = micros() - start;
      if (elapsed > _maxReadMicros) _maxReadMicros = elapsed;
      return -1;
    }
    const uint32_t elapsed = micros() - start;
    if (elapsed > _maxReadMicros) _maxReadMicros = elapsed;
    return 0;
  }

  const uint32_t count =
      (_chunkSamples < maxSamples) ? _chunkSamples : maxSamples;
  const int16_t* source = _buffers[_readSlot];
  for (uint32_t i = 0; i < count; ++i) dest[i] = source[i];

  _readSlot ^= 1u;
  --_ready;
  ++_chunksDelivered;

  // The buffer just drained is now free, and _writeSlot already points at it:
  // the two slots alternate in lockstep.
  if (!topUpQueue()) {
    const uint32_t elapsed = micros() - start;
    if (elapsed > _maxReadMicros) _maxReadMicros = elapsed;
    return -1;
  }

  const uint32_t elapsed = micros() - start;
  if (elapsed > _maxReadMicros) _maxReadMicros = elapsed;
  return static_cast<int>(count);
}

void M5MicrophoneCapture::requestStop() {
  // NON-BLOCKING, deliberately. Stop queueing new reads and hand over to the
  // caller's cooperative drain.
  //
  // This used to spin on M5.Mic.isRecording() for up to 60 ms with M5.delay(1)
  // inside App::tick(). That, plus M5.Mic.end() waiting for the task to exit,
  // is what produced 56-90 ms loop iterations. Nothing here waits any more.
  if (_state != State::Recording) return;
  _state = State::Stopping;
}

bool M5MicrophoneCapture::isDrained() const {
  // The task has finished with both buffers once nothing is left queued.
  // Reading an atomic counter; no waiting.
  return M5.Mic.isRecording() == 0;
}

void M5MicrophoneCapture::finishStop() {
  if (_state == State::Idle) return;
  _state = State::Idle;

  diag::log().printf("[mic] stopped: %lu chunks delivered, %lu queue stalls\r\n",
                static_cast<unsigned long>(_chunksDelivered),
                static_cast<unsigned long>(_queueStalls));

  _inFlight = 0;
  _ready = 0;
}

}  // namespace tth
