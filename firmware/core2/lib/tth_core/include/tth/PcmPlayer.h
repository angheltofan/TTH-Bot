#pragma once

#include <stdint.h>

#include "tth/AudioBus.h"
#include "tth/AudioFormat.h"

// Non-blocking PCM playback of one response stream.
//
// THE SPEAKER NON-BLOCKING INVARIANT
//
// M5Unified's Speaker has two request slots per channel, and playRaw() --
// like Mic.record() -- SPINS until the speaker task frees one
// (Speaker_Class.cpp _set_next_wav: `for (;;) { ... xSemaphoreTake(1); }`).
// Calling it with both slots occupied would stall App::tick() for a whole
// chunk, which is the 32 ms microphone-priming stall again on the output side.
//
// So: NO CODE PATH MAY QUEUE AUDIO UNLESS THE SPEAKER HOLDS FEWER THAN
// kSpeakerQueueDepth REQUESTS. This class checks slotsOccupied() immediately
// before every play(), and the device adapter re-checks it immediately before
// the one playRaw() call in the firmware. The tests use a speaker fake that
// fails the test outright if it is ever asked to queue a third request.
//
// BUFFERS
//
// A fixed ring of three preallocated slots (M5Unified's own advice for runtime
// data). A slot handed to the speaker stays untouched until the speaker has
// finished with it -- the speaker task reads it from its own task, which is
// the Phase 4 lesson about asynchronous buffer lifetimes. Nothing allocates.
//
// FORMAT
//
// A stream declares its format at openStream() and every chunk must match it
// exactly. A mismatch is REJECTED (FormatMismatch), never resampled or played.
// The sample rate given to the speaker is always the stream's, explicitly, so
// 16 kHz loopback and 24 kHz assistant speech each play at their own rate.
//
// Portable: the hardware is behind ISpeakerOutput, the bus behind AudioBus.

namespace tth {

// M5Unified: `_ch_info[ch].wavinfo[2]`.
const uint32_t kSpeakerQueueDepth = 2;

// The speaker, as PcmPlayer sees it. Implemented by M5SpeakerOutput on the
// device and by a strict fake in the tests.
class ISpeakerOutput {
 public:
  virtual ~ISpeakerOutput() {}
  // Requests the speaker task currently holds, 0..kSpeakerQueueDepth. A COUNT,
  // like Mic.isRecording(). MUST NOT block.
  virtual uint32_t slotsOccupied() const = 0;
  // Queues one buffer at `sampleRate`. Called only when slotsOccupied() <
  // kSpeakerQueueDepth. MUST NOT block; returns false if refused.
  virtual bool play(const int16_t* samples, uint32_t count,
                    uint32_t sampleRate) = 0;
  // Asks the speaker to stop this channel. MUST NOT wait for it.
  virtual void stop() = 0;
};

enum class PlayerState : uint8_t {
  // No stream; the bus is not owned by the player.
  Idle = 0,
  // Accepting chunks and playing them.
  Streaming,
  // No more chunks will come; playing out what is queued.
  Ending,
  // Cancelled: the speaker has been told to stop and still holds requests.
  Stopping,
  // The hardware has finished with every slot. The speaker is STILL owned --
  // release() hands it back. Kept separate so a barge-in can cancel the turn
  // source between "the speaker is quiet" and "the speaker is released".
  Drained,
};

const char* toString(PlayerState state);

enum class PlayerPush : uint8_t {
  // Copied into the ring. The caller may consume those samples.
  Accepted = 0,
  // Ring full. Nothing was taken; offer the same samples again later.
  Busy,
  // The chunk's format is not the stream's. Refused, never played.
  FormatMismatch,
  // No stream is open, or it is ending/stopping.
  NotOpen,
};

const char* toString(PlayerPush result);

enum class PlaybackEnd : uint8_t {
  None = 0,
  Completed,
  Cancelled,
  Error,
};

const char* toString(PlaybackEnd reason);

struct PlaybackMetrics {
  uint32_t queued;          // chunks accepted by submit()
  uint32_t played;          // chunks the speaker finished
  uint32_t samplesQueued;
  uint32_t underruns;       // episodes of running dry mid-stream
  uint32_t formatRejects;
  uint32_t playRefusals;    // play() returned false
  uint32_t drainTimeouts;
  float maxLevel;
  // Largest |sample| accepted by submit(): the player's INPUT level, i.e.
  // exactly what the source handed over, after any source-side gain.
  int32_t inputPeak;
};

class PcmPlayer {
 public:
  static const uint32_t kSlots = 3;

  PcmPlayer(AudioBus& bus, ISpeakerOutput& output, uint32_t drainTimeoutMs);

  // Hands over three preallocated slots of `slotSamples` each. Once, at
  // start-up.
  void begin(int16_t* slotA, int16_t* slotB, int16_t* slotC,
             uint32_t slotSamples);

  // Opens a stream in `format` and acquires the speaker through AudioBus.
  // Refused if not Idle, if the format is not playable, if no slots are
  // attached, or while the MICROPHONE still owns the bus -- a capture that is
  // still draining must finish before the speaker may start.
  bool openStream(const AudioFormat& format, uint32_t nowMs);

  // A chunk would be accepted right now.
  bool canAccept() const;

  // Copies up to one slot's worth of the chunk into a free slot and reports
  // how many samples it took in `accepted` -- the caller consumes exactly that
  // many and offers the rest again. Never blocks, never plays anything.
  PlayerPush submit(const AudioChunk& chunk, uint32_t& accepted);

  uint32_t slotSamples() const { return _slotSamples; }

  // No more chunks will come. Plays out the ring, then Drained.
  void markEndOfStream(uint32_t nowMs);

  // Stops at once: refuses further chunks, drops everything not yet handed to
  // the speaker, and asks the speaker to stop. Slots the speaker still holds
  // are NOT reused until it lets go. Ends in Drained.
  void cancel(uint32_t nowMs, PlaybackEnd reason);

  // Once per loop. Retires finished slots, queues filled ones (only while
  // fewer than kSpeakerQueueDepth are held), detects underruns, and advances
  // Ending/Stopping to Drained. Never blocks.
  void service(uint32_t nowMs);

  // Drained -> Idle, releasing the speaker through AudioBus. The ONE place the
  // player gives the bus back, whether the stream completed, was cancelled or
  // failed. Returns false (and does nothing) in any other state.
  bool release();

  // True exactly once per stream: the first time the speaker ACCEPTED audio.
  // This is when the haptic pulse fires -- not when speech was announced.
  bool consumeFirstAudio();

  // 0..1 RMS of the chunk the speaker is playing NOW (the oldest it holds),
  // or 0 when it holds nothing. Measured from the samples themselves.
  float currentLevel() const;

  PlayerState state() const { return _state; }
  bool isIdle() const { return _state == PlayerState::Idle; }
  bool isDrained() const { return _state == PlayerState::Drained; }
  bool isStreaming() const { return _state == PlayerState::Streaming; }
  PlaybackEnd endReason() const { return _endReason; }
  const AudioFormat& format() const { return _format; }
  const PlaybackMetrics& metrics() const { return _metrics; }
  // Wall time from end-of-stream/cancel until the hardware was quiet.
  uint32_t lastDrainMs() const { return _lastDrainMs; }
  uint32_t filledSlots() const { return _filled; }
  uint32_t playingSlots() const { return _playing; }

 private:
  enum class SlotState : uint8_t { Free = 0, Filled, Playing };

  void retireFinished(uint32_t occupied);
  void queueFilled();
  void enterDrained(uint32_t nowMs);
  static uint32_t next(uint32_t index) { return (index + 1) % kSlots; }

  AudioBus& _bus;
  ISpeakerOutput& _output;
  const uint32_t _drainTimeoutMs;

  int16_t* _slots[kSlots];
  uint32_t _slotSamples;
  uint32_t _slotCount[kSlots];
  float _slotLevel[kSlots];
  SlotState _slotState[kSlots];

  // Ring indices. Filled slots are queued in order from _playIdx; the oldest
  // slot the speaker holds is _doneIdx.
  uint32_t _fillIdx;
  uint32_t _playIdx;
  uint32_t _doneIdx;
  uint32_t _filled;
  uint32_t _playing;

  PlayerState _state;
  PlaybackEnd _endReason;
  AudioFormat _format;
  bool _firstAccepted;
  bool _firstAudioPending;
  bool _inUnderrun;
  uint32_t _drainStartedMs;
  bool _drainClockRunning;
  uint32_t _lastDrainMs;
  PlaybackMetrics _metrics;
};

}  // namespace tth
