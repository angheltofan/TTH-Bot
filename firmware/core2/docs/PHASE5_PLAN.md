# Phase 5 — playback, haptics, barge-in (offline)

Status: **COMPLETE — validated on the physical M5Stack Core2.** Loopback and
synthetic playback, backpressure (`received == committed`, `gaps=0`),
`underruns=0 rejects=0 refusals=0`, barge-in, haptics, face, stable memory,
and loudness at master volume 255 all confirmed. One hardware observation:
a speaker whine present only on USB power, absent on battery — USB power-path
noise, not a firmware defect; no DSP filtering by design (README, "Power
supply and audible whine"). Phase 6 waits for a reviewed plan.

---

## Invariants (added at approval, enforced in code)

These four are not design intentions — each is enforced at a single point in
the code and pinned by a named test.

### I-1. TurnBuffer lifetime

The TurnBuffer must not be reset, reused or invalidated while any reader still
has unconsumed audio. After recording ends, TurnStreamer continues draining
`[cursor, committedLength)` until every committed sample is accepted, or the
turn is explicitly abandoned.

- **Mechanism:** a reader lease. `TurnBuffer::retain()` / `release()`;
  `reset()` returns false and changes nothing while any lease is held.
  TurnStreamer holds one from `begin()` until Finished/Failed/abandon;
  loopback playback holds one for the whole response until the last chunk or
  `cancel()`.
- **Enforced at:** `TurnBuffer::reset()`, and `CaptureController::start()`,
  which refuses with `CaptureStopReason::BufferInUse` *before* touching the
  buffer, the metrics or the bus.
- **Tests:** `test_turn_buffer` (held lease blocks reset and changes nothing;
  every reader must let go), `test_turn_stream`
  (`the_buffer_cannot_be_reset_until_the_tail_is_sent`), `test_capture`
  (`start_is_refused_while_a_reader_holds_the_buffer`), `test_barge_in`
  (`the_microphone_cannot_start_over_a_buffer_still_being_played`).

### I-2. Committed-length boundary

Readers only read samples that are fully committed. Ordering:
**write samples → publish new length → reader may observe/read them.**

- **Mechanism:** `TurnBuffer::append()` copies the samples, *then* publishes
  `_committed` with a release store. Readers use only `committedSamples()`
  (acquire load). The writer's own `_size` is never read by a reader.
  Everything runs on the loop today, but the ordering is expressed in the
  memory model, not left to "it happens to be one thread" — Phase 6 may not
  be.
- **Also:** while the recording is live, TurnStreamer sends only whole frames;
  a short tail waits until the producer is done.
- **Tests:** `test_turn_buffer` (`the_committed_length_follows_every_append`),
  `test_turn_stream` (`only_committed_samples_are_ever_read` — the sink checks
  every chunk against the committed length at the moment of the push).

### I-3. Speaker non-blocking rule

**No code path may call `M5.Speaker.playRaw()` unless
`M5.Speaker.isPlaying(ch) < 2`.**

- **Mechanism:** checked twice, deliberately. `PcmPlayer::queueFilled()`
  re-reads `slotsOccupied()` immediately before every `play()`; and
  `M5SpeakerOutput::play()` — the **only** `playRaw()` call site in the
  firmware — re-checks `isPlaying(ch)` immediately before the call. It also
  refuses if `!M5.Speaker.isRunning()`, because `playRaw()` would otherwise
  call `Speaker.begin()` itself, behind AudioBus.
- **Observable:** `[pb] guard=<busy>/<notRunning>` in the heartbeat. Both
  must stay 0; non-zero means a caller broke the contract and the guard
  caught it.
- **Tests:** every playback suite uses a strict speaker fake that fails the
  test outright if asked to queue a third request.

### I-4. Measurements, not assumptions

The speaker priming cost (§0.2) and the bar-vs-sound lead (§4) are **not**
designed around. They are instrumented (`spk.playRaw#1`, `playback.open`,
`M5.Speaker.stop`, `barge.*`, `maxLoopSpeaking`) and reported by the physical
test.

### I-5. Loopback ownership (no borrowed pointer is kept)

`ITurnSource::pushUserAudio()` lends its pointer for that call only.
`LocalMockTurnSource` keeps **nothing** of it — not the pointer, not the chunk
— only a count, `_received`.

**Where accepted loopback samples live until PcmPlayer consumes them:** in the
PSRAM TurnBuffer, referenced through the mock's **own explicit lease**:

| Moment | Who holds a lease |
|---|---|
| `TurnStreamer::begin()` → `beginUserTurn()` | streamer **and** mock (overlap starts) |
| recording, streaming | streamer and mock |
| streamer finishes: `endUserTurn()`, *then* streamer releases | mock |
| WAITING think delay | mock |
| response: `peek` → `PcmPlayer::submit()` copies → `consume` | mock |
| last `consumePlayback()` (final chunk already copied into a player slot) | nobody |
| `cancel()` in any phase | nobody |

- The lease is taken in `beginUserTurn()`, while the streamer still holds its
  own, so there is **no instant** between recording and replay when the buffer
  is unprotected. (The first implementation took it in `respond()`, leaving
  the think delay covered only by App's "ignore presses in WAITING" policy —
  corrected before the physical test.)
- `pushUserAudio()` accepts a chunk **only** if it is exactly the next
  committed range of the leased buffer (`samples == data() + _received`,
  `_received + count <= committedSamples()`). An identical copy at another
  address, a gap, a repeat or uncommitted samples is `Fatal`. So every
  accepted sample lies in lease-protected memory by construction.
- Replay addresses are re-derived from the leased buffer
  (`data() + cursor`), never from a pushed pointer; the response length is
  exactly `_received`.
- The mock's mode is latched per user turn, so switching it mid-turn cannot
  drop the lease of a loopback turn that will still replay.
- Synthetic turns read nothing from the recording and take no lease.

**Tests:** `test_loopback_ownership` — chunk objects scribbled over after
every push with exact replay; copies/gaps/repeats/uncommitted refused;
lease held from first sample to last copy with the player's copy surviving
buffer reuse; `CaptureController::start()` refused with `BufferInUse` during
think and response, allowed after the last copy; `cancel()` releases in every
phase; mode latching; synthetic holds none.

---

## Implementation refinements (vs. the approved text below)

Recorded so the plan and the code do not disagree:

1. **Audio pull is peek/consume, not a single read.** `peekPlaybackChunk()`
   returns a view that stays valid until the next non-const call on the
   source; `consumePlayback(n)` advances it. That makes a full player ring
   lossless (peek, fail to submit, peek the same samples next loop) — the
   inbound mirror of the TurnStreamer cursor.
2. **`PcmPlayer` is portable** (`lib/tth_core`), with the hardware behind
   `ISpeakerOutput`; the device adapter is `src/audio/M5SpeakerOutput`. This
   puts the ring, the guard and the drain logic under host tests.
3. **Bar level is the chunk being *heard*,** not the chunk being queued: the
   RMS of the oldest slot the speaker still holds. This removes most of the
   predicted queue lead; what remains is M5Unified's internal DMA latency,
   which is measured, not assumed.
4. **Stream metrics are a separate `[stream]` line**, not part of SUMMARY2:
   the stream may legitimately outlive the capture (I-1), so it reports when
   *it* finishes.
5. **The event queue refuses the newest event when full** (counted), instead
   of dropping the oldest: lifecycle events must never be silently replaced.
6. **Barge-in is a portable sequencer** (`BargeIn` + `IBargeInOps`), so the
   order is recorded by tests rather than implied by line order in App. If the
   button is released during the speaker drain, the microphone is not started.
7. **No `onCancel()`**: `onPress()` from Speaking is barge-in, and
   `onTurnComplete()` covers a barge-in abandoned by an early release.
8. **Synthetic scratch** is `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`, not DMA:
   only our code touches it; the player copies out of it.
9. **`platformio.ini` is unchanged.** The mock mode lives in `Config.h` with
   an `#ifndef` default, and is switchable at run time (`m`).

Scope: `LocalMockTurnSource` (16 kHz loopback + deterministic 24 kHz
synthetic), `PcmPlayer`, real-amplitude cheek bars, 120 ms haptics,
cancellation and barge-in. **No Wi-Fi, TLS, WebSocket, Gemini or Supabase.**

---

## 0. Two facts read out of M5Unified 0.2.21 that shape the whole design

### 0.1 The speaker's sample rate is per call, not per `begin()`

`Speaker_Class.hpp:217`

```cpp
bool playRaw(const int16_t* raw_data, size_t array_len,
             uint32_t sample_rate = 44100, bool stereo = false,
             uint32_t repeat = 1, int channel = -1,
             bool stop_current_sound = false);
```

`sample_rate` travels with the buffer into `wav_info_t::sample_rate_x256`.
Nothing in `speakerBegin()` fixes a rate. **Native-rate playback is therefore
genuinely supported with no hardware reconfiguration** — which is what makes
correction 1's preferred option ("configurable native-rate playback") the
cheap one rather than the expensive one. No resampler is needed for Phase 5.

### 0.2 `playRaw` blocks exactly the way `record` does

`Speaker_Class.cpp::_set_next_wav`:

```cpp
for (;;) {
  ...
  if (phase == wav_phase_empty || ...) { /* claim slot, return true */ }
  ...
  xSemaphoreTake(_task_semaphore, 1);   // <-- spins until the task frees a slot
}
```

Two slots per channel, and the task frees one only when it finishes playing it.
**This is the 32 ms mic-priming stall again, on the speaker side.** Calling
`playRaw` with both slots occupied would block for a whole chunk duration
inside `App::tick()`.

The rule that falls out, and the single most important line in this plan:

> **`PcmPlayer` never calls `playRaw` unless `M5.Speaker.isPlaying(ch) < 2`.**

`isPlaying(uint8_t channel)` (`Speaker_Class.hpp:113`) returns the *count* of
occupied slots and does not block — the same shape as `Mic.isRecording()`, and
the same trap: it is a count, not a bool. With that guard, playback is
non-blocking by construction, not by hope.

M5Unified's own note (`Speaker_Class.hpp:175`): *"either have three buffers and
use them in sequence, or two buffers and use them alternately"*. The ring below
uses **three**.

---

## 1. The format contract (correction 1)

New portable header `lib/tth_core/include/tth/AudioFormat.h`:

```cpp
enum class SampleEncoding : uint8_t { S16LE = 0 };

struct AudioFormat {
  uint32_t sampleRate;
  uint8_t  channels;
  SampleEncoding encoding;
};

constexpr AudioFormat kCaptureFormat{TTH_MIC_SAMPLE_RATE, 1, SampleEncoding::S16LE};
constexpr AudioFormat kAssistantFormat{TTH_SPEAKER_SAMPLE_RATE, 1, SampleEncoding::S16LE};

bool isCompatible(const AudioFormat& a, const AudioFormat& b);
bool isPlayable(const AudioFormat& f);   // mono + S16LE + rate in [8000, 48000]
```

Every chunk carries it — a borrowed view, never an owner:

```cpp
struct AudioChunk {
  const int16_t* samples;   // BORROWED. Valid only for the duration of the call.
  uint32_t count;
  AudioFormat format;
};
```

`PcmPlayer::openStream(const AudioFormat&)` declares the rate for one response
and is the only place a rate enters playback. Then:

- `submit()` **rejects** a chunk whose format is not bit-identical to the open
  stream's — returns `PlayerPush::FormatMismatch`, logs once, faults the turn.
  It does not resample, does not coerce, does not play it anyway.
- `openStream()` rejects a format failing `isPlayable()`.
- There is no default rate anywhere. `playRaw` is always called with
  `_stream.sampleRate` explicitly.

So loopback opens at 16 000 and synthetic opens at 24 000, and 16 kHz samples
can only reach hardware configured for 16 kHz. The existing `Config.h` comment
warning that the two rates must never be crossed becomes an enforced invariant
rather than a comment.

`TTH_SPEAKER_SAMPLE_RATE` stays as the *assistant* rate. It is no longer "the
speaker's rate", and the comment says so.

---

## 2. Live streaming and backpressure (correction 2)

### The shape

```
M5.Mic ──chunk──▶ CaptureController::poll()
                      │ removeDcOffsetAndMeasure()   (in place, as today)
                      ▼
                  TurnBuffer.append()     ← the durable record, PSRAM, as today
                      │
                      ▼
                  TurnStreamer::service()  ← reads a CURSOR into that same buffer
                      │
                      ▼
                  ITurnSource::pushUserAudio(AudioChunk)   → Phase 6: WebSocket
```

**The TurnBuffer is the backpressure reservoir, and the streamer holds a read
cursor into it.** This is the design decision worth arguing for, because it
resolves three of the requirements at once:

- *Streams live* — the cursor advances every `poll()` while capture is active.
  It never waits for the recording to finish; at end of turn it is typically
  already at the head.
- *No second buffer* — nothing is copied. `pushUserAudio` is handed
  `_turn.data() + _cursor`. There is no 45-second duplicate, and no allocation.
- *Never silently dropped* — if the sink is busy the cursor simply does not
  advance. The samples are still in PSRAM; they are sent later.

The alternative (push the scratch chunk straight through and hope) cannot
satisfy the third: `CaptureController::_chunk` is reused on the next
`readChunk()`, so a `Busy` result would either lose the chunk or force us to
stop re-queueing the mic — and *that* loses audio at the hardware level,
silently, which is the one thing forbidden.

### Backpressure

```cpp
enum class PushResult : uint8_t { Accepted, Busy, Fatal };
```

- `Accepted` — cursor advances by `count`.
- `Busy` — cursor unchanged, retried next loop. Counted as `pushRetries`.
- `Fatal` — turn aborts to ERROR with a named reason. Never ignored.

Bounded by construction: the cursor can lag at most `TurnBuffer::capacity()`
behind, and when the buffer fills, the existing `CaptureStopReason::BufferFull`
path stops the turn with an explicit reason. Nothing new can overrun.

New per-turn metrics, reported in SUMMARY2: `streamed=…B lag=…B retries=…`.
A healthy turn ends with `lag=0`.

### Streaming frame size

320 samples = 20 ms at 16 kHz, matching the Flutter app's frame size so the
gateway sees identical traffic from either embodiment. One `service()` call
pushes at most `TTH_STREAM_MAX_FRAMES_PER_POLL` (2) frames, so streaming cannot
itself become a loop overrun.

---

## 3. Callback and thread safety (correction 3)

**No callbacks.** `ITurnSource` is polled and pulled, never pushing into App.

```cpp
enum class TurnEventType : uint8_t { None, SpeechStart, TurnComplete, Error };

struct TurnEvent {          // small value type, copied by value
  TurnEventType type;
  TurnError error;          // enum, not a pointer
};

class ITurnSource {
 public:
  virtual void beginUserTurn(const AudioFormat&) = 0;
  virtual PushResult pushUserAudio(const AudioChunk&) = 0;
  virtual void endUserTurn() = 0;
  virtual void cancel() = 0;
  virtual void poll(uint32_t nowMs) = 0;

  // Control events: bounded queue, drained by App in the cooperative loop.
  virtual bool nextEvent(TurnEvent& out) = 0;

  // Audio: PULLED, never pushed.
  //   true  -> `out` borrows the source's memory for THIS CALL ONLY.
  virtual bool readPlaybackChunk(AudioChunk& out) = 0;
};
```

Why pull rather than push: an `onAudioChunk` callback has to answer "who owns
this pointer, and until when?" — and in Phase 6 it would be answering that from
the network task, mutating App/FaceRenderer/AudioBus off-loop. Pulling makes
the answer trivial and permanent:

> **Ownership rule (documented at the interface, asserted in tests):**
> `AudioChunk::samples` is BORROWED. It is valid only until the call returns.
> A consumer that needs to keep the data must copy it before returning.
> `PcmPlayer::submit()` copies into its own ring, which is why it may keep it.

The control-event queue is a bounded `TurnEventQueue` (8 slots, value types,
drop-oldest with a counter — control events are diagnostics-adjacent; audio is
never in it). App drains it in `tick()`, and App remains the only thing that
touches `_machine`, `_faceRenderer`, `_audioBus` and `_player`.

`LocalMockTurnSource` is single-threaded and polled, so in Phase 5 this is
"free". It is specified now precisely so that Phase 6's `GatewayTurnSource` —
which *will* have a network task — has no legal way to reach into App: it
writes into its own preallocated ring, publishes with a release store, and the
loop pulls.

---

## 4. PcmPlayer (correction 4)

`src/audio/PcmPlayer.{h,cpp}` (device) + portable ring/state logic in
`PlaybackQueue` so it can be tested on the host.

**Buffers — preallocated once at start-up, in `allocateAudioBuffers()`:**

three chunks × 960 samples (40 ms at 24 kHz / 60 ms at 16 kHz) × 2 bytes =
**5 760 bytes**, `MALLOC_CAP_DMA`, each verified with `isDmaCapable()` before
first use. Same cap and same guard as the mic chunks — the speaker task reads
these buffers from its own task, and the Phase 4 LoadStoreError is the reason
`MALLOC_CAP_INTERNAL` is not good enough. **Nothing allocates during playback.**

**Non-blocking service:**

```
service(nowMs):
  occupied = M5.Speaker.isPlaying(TTH_SPEAKER_CHANNEL)   // count, 0..2
  if occupied >= 2: return                               // never block in playRaw
  if no filled ring slot ready:
      if _started and occupied == 0 and not _endOfStream: ++_underruns
      return
  playRaw(slot, count, _stream.sampleRate, false, 1, TTH_SPEAKER_CHANNEL, false)
  ++_played ; advance ring ; first-accept → haptics latch
```

`submit(AudioChunk)` copies into a free ring slot and returns
`Accepted` / `Busy` (ring full — caller retries, nothing dropped) /
`FormatMismatch`.

**Counters:** `queued`, `played`, `underruns`, `formatRejects`, plus
`maxSubmitMicros` and `maxServiceMicros` through the existing `TTH_TIME_BLOCK`,
reported in the heartbeat and the turn summary.

**Amplitude — from samples actually going to the speaker.** `service()` runs the
existing `measureLevels()` over the slot at the moment it is handed to
`playRaw`, and feeds the existing `SpeechLevelSmoother` (55 ms attack / 190 ms
release, unchanged) which already drives the cheek bars.
`simulatedSpeechAmplitude()` stays, demoted to the `s` diagnostic only.

Honest caveat, to be measured not assumed: with two slots occupied the bars lead
the sound by up to one chunk (~40 ms). That is below the ~100 ms threshold where
lip-sync reads as wrong, but the physical test will confirm it rather than the
plan asserting it.

**The smile does not change.** Nothing in Phase 5 touches `FaceGeometry`,
`FaceFrame` or the lower-face sprite layout. Only `speechLevel` gets a real
source instead of a simulated one — one number changes provenance, no rendering
code changes.

**Release:** `AudioBus::releaseAll()` on completion, cancellation and error —
one `finishPlayback()` path all three funnel through, so there is no way to
leave the speaker installed. Same three-step shape as capture:
`requestStop()` → `isDrained()` (`isPlaying(ch) == 0`) → `finishStop()`, polled
across iterations. `M5.Speaker.end()` is expected to wait for its task the way
`M5.Mic.end()` did (1.0–1.7 ms measured); it will be timed with
`TTH_TIME_BLOCK` and reported, not assumed.

---

## 5. Haptics (correction 5)

`src/haptics/Haptics.{h,cpp}` — `pulse(nowMs)` sets
`M5.Power.setVibration(TTH_VIBRATION_LEVEL)` and a deadline;
`poll(nowMs)` clears it after `TTH_VIBRATION_MS` (120). No `delay()`.

Fired from **one** place: inside `PcmPlayer::service()`, on the first `playRaw`
that returns true for the stream — i.e. when samples are actually accepted for
playback, not when `SpeechStart` is announced. Latched per stream
(`_hapticFired`), so a 20-chunk response produces exactly one 120 ms pulse.
Cleared by `openStream()`.

Test: a fake speaker accepting 20 chunks yields exactly one pulse; a stream
announced but never accepting a chunk yields none.

---

## 6. Barge-in (correction 6)

Implemented in `App::bargeIn(nowMs)`, in exactly the specified order, each step
a named `ScopedStage` so the breakdown appears in `[blocks]` the way
`capture.start` now does:

| # | Step | Stage label |
|---|---|---|
| 1 | `_player.closeStream()` — stop accepting chunks, drop the ring | `barge.closeStream` |
| 2 | `_player.requestStop()`, drain `isPlaying(ch)==0`, `finishStop()` | `barge.drainSpeaker` |
| 3 | `_turnSource.cancel()` — no later event or chunk may be delivered | `barge.cancelSource` |
| 4 | `AudioBus::acquireMic()` (speaker `end()` then mic `begin()`) | `barge.acquireMic` |
| 5 | `_capture.start()` | `barge.startCapture` |
| 6 | **only on success** → `LISTENING` | — |

Step 2 is cooperative: if the speaker does not drain immediately the machine
sits in a `BargingIn` sub-state and retries next loop, with the
`TTH_CAPTURE_DRAIN_TIMEOUT_MS` deadline. The face does **not** show LISTENING
until step 5 succeeds; on failure it goes to ERROR. Steps 4–5 include the known
32 ms mic priming, so the whole transition is expected around 35–50 ms and is
counted as a *transition* iteration, not steady state.

Reported as `bargeTotalUs` plus the per-stage breakdown, so
"speaker-to-microphone transition" is a measured number.

Post-cancel safety: `cancel()` bumps a `_generation` counter; events and chunks
carrying a stale generation are discarded and counted (`staleEvents`). That is
what makes "no late delivery" enforceable rather than merely intended, and it
is what Phase 6's network path will need anyway.

---

## 7. State machine changes

`ConversationStateMachine` gains `onSpeechStart()`, `onTurnComplete()`,
`onCancel()`; `onPress()` accepts `Speaking` (→ barge-in). The Phase 2
stand-in `TTH_PHASE2_WAITING_MS` and its fallback are **deleted** — WAITING now
ends because the turn source delivered speech, which is what it always meant.

```
READY   --press-->        LISTENING
LISTENING --release-->    WAITING            (endUserTurn)
WAITING --SpeechStart-->  SPEAKING           (openStream; haptics on first chunk)
SPEAKING --TurnComplete + ring drained + speaker released--> READY
SPEAKING --press-->       LISTENING          (barge-in, ordered as above)
any --Error-->            ERROR --press--> retry
```

---

## 8. Mock turn source

`LocalMockTurnSource`, mode from `TTH_MOCK_TURN_MODE`, switchable live by a
serial key so both can be checked in one session:

- **Loopback** — after `endUserTurn()`, waits `TTH_MOCK_THINK_MS` (600) so
  WAITING is visible, then replays the captured `TurnBuffer` at its **native
  16 kHz**, `readPlaybackChunk()` handing out 960-sample windows straight from
  PSRAM. No copy, no resample, no allocation. Proves the whole
  mic → buffer → speaker → amplitude → bars chain end to end.
- **Synthetic** — a deterministic 24 kHz envelope (the quiet/medium/loud/silence
  shape `simulatedSpeechAmplitude()` already defines), generated one chunk at a
  time into a single preallocated scratch buffer. Repeatable, works with no mic,
  and exercises the 24 kHz path that the real assistant will use.

Loopback is the default: it is the one that would catch a sample-rate mistake
audibly.

---

## 9. Memory

| Item | Bytes | Region |
|---|---:|---|
| Playback ring, 3 × 960 × 2 | 5 760 | DMA DRAM |
| Synthetic scratch, 960 × 2 | 1 920 | DMA DRAM |
| Turn event queue, 8 × 4 | 32 | DRAM |
| **Phase 5 internal total** | **≈ 7.7 KB** | |

Loopback adds nothing (it reads the existing PSRAM turn buffer). So Phase 5 is
cheap in the region that matters, and the sprite-placement question stays open
for Phase 6 exactly as agreed — the `[mem]` line (free internal / largest block
/ min free / free PSRAM / sprite region) is the evidence, now also sampled
during playback, which is the new peak.

---

## 10. Files

**New portable (`lib/tth_core/`)** — `AudioFormat.h`, `ITurnSource.h`,
`TurnEventQueue.{h,cpp}`, `TurnStreamer.{h,cpp}`, `PlaybackQueue.{h,cpp}`
(ring/state logic, hardware-free), `LocalMockTurnSource.{h,cpp}`.

**New device (`src/`)** — `audio/PcmPlayer.{h,cpp}`, `haptics/Haptics.{h,cpp}`.

**Modified** — `CaptureController` (optional `ITurnSource*` sink + streamer
hook), `ConversationStateMachine` (three events), `App` (event drain, playback
service, barge-in, haptics poll, buffer allocation), `CaptureSummary`
(stream metrics), `Config.h`, `platformio.ini` (mock mode flag), `README.md`.

**Untouched** — `FaceGeometry`, `FaceFrame`, `FaceAnimator`, `FaceRenderer`,
`FaceOverride`, `PttButton`, `AudioBus`, `TurnBuffer`, `LogQueue`,
`LogDrainer`, `M5MicrophoneCapture`, and every Flutter/Supabase file.

---

## 11. Tests (target ≈ 55 new cases, all 190 existing still green)

**`test_audio_format`** — compatibility is exact; 16 kHz chunk into a 24 kHz
stream is rejected, not coerced (**the central regression test of this phase**);
stereo rejected; unplayable rate rejected.

**`test_turn_stream`** — cursor advances during capture, not only at the end;
`Busy` leaves the cursor unchanged and the same samples are re-offered; every
captured sample is delivered exactly once, in order; `Fatal` aborts; nothing is
copied into a second buffer (the pushed pointer aliases the turn buffer);
cursor lag is bounded by capacity.

**`test_playback`** — never submits with 2 slots occupied (a fake speaker fails
the test if asked); ring full returns `Busy` and drops nothing; underrun
counted only mid-stream; counters; amplitude comes from the played slot;
release on complete, cancel and error; no allocation after `begin()`.

**`test_turn_events`** — bounded queue; drops counted; FIFO; a borrowed chunk
pointer is not retained past the call (fake source poisons its buffer after
return and the consumer's copy must still be intact).

**`test_barge_in`** — exact step order recorded by fakes; LISTENING only after
capture succeeds; ERROR on acquire failure; no event or chunk delivered after
`cancel()` (generation check); speaker always released.

**`test_haptics`** — exactly one pulse per stream, at first accepted chunk;
none if no chunk is ever accepted; 120 ms; non-blocking.

Plus extensions to `test_conversation_state` for the three new events.

---

## 12. What I will measure on the physical test

1. `speaker.begin` and the **first** `playRaw` — §0.2 predicts a priming cost
   analogous to the mic's 32 ms. Measured, then documented, not designed around
   in advance.
2. Full barge-in speaker→mic transition, per stage.
3. `maxLoopSteady` during playback — the budget claim of this whole design.
4. Underruns across a 45 s loopback turn.
5. `[mem]` during playback — the Phase 6 sprite-placement input.

---

## 13. Out of scope, explicitly

Wi-Fi, TLS, WebSocket, `GatewayTurnSource`, Gemini, Supabase, provisioning,
the 16→24 kHz resampler (not needed — §0.1), and any change to the Flutter app.

---

## 14. Post-test correction: loopback loudness

Physical test: functionally passed; recorded-voice loopback too quiet.

**Diagnosis (from M5Unified 0.2.21 source, confirmed by the boot report):**
- Mic: 16 kHz, magnification 16, over_sampling 1, noise filter 0 — the
  generic internal-mic setup; the Core2 case sets pins only.
- Capture: DC removal only. Stream, TurnBuffer, PcmPlayer: bit-exact copies.
- Speaker mixer (`Speaker_Class.cpp`): `volume = magnification × master²`,
  times `channel²`; per 16-bit mono sample the amplitude factor is
  16·V²·255²/2³⁶, then `>> 8` and a hard int16 clamp. V = 96 → ×0.140
  (−17.1 dB); V = 160 → ×0.388 (−8.2 dB); V = 255 → ×0.984. The mixer never
  amplifies, so full scale cannot clip digitally at any volume; the limits of
  a high volume are analog (1 W speaker, NS4168, battery current).
- Synthetic speech is generated at ≈ −5 dBFS; a voice recording sits much
  lower. Master volume scales both (and future Gemini audio) equally, so it
  is the wrong lever. **The recording needs its own gain.**

**Correction (`tth/PcmGain.h`, used only by `LocalMockTurnSource` loopback):**
turn-level gain fitted to the 99.9th-percentile level (never above the
configured `TTH_LOOPBACK_GAIN_DB`, never below unity), 32-bit amplification
with the gain capped at 18 dB so products cannot wrap, a memoryless soft knee
above −3 dBFS, int16 saturation. No per-chunk normalisation; output is a pure
per-sample function of the turn's single gain, identical under any chunking.
0 dB is an exact zero-copy bypass. Synthetic and the player path are untouched.

**Metrics:** `[loopback] turn level` per response; `[play] SUMMARY2` with
`gainDb`, `inputPeak` (source, pre-gain), `outputPeak` (what PcmPlayer
received), `limitedSamples`. `PlaybackMetrics::inputPeak` is new.

**Tests:** `test_loopback_gain` (16 cases), plus playback-summary fit tests in
`test_summary`. The final gain is chosen by the A/B test in the README.

## 15. Post-test correction: speaker master volume A/B

The loopback-gain A/B showed the gain works but the audible change was small:
`inputPeak` 0.69–0.78, `outputPeak` 0.88–0.93, applied gain +3.3 to +7.1 dB
(the turn fit held it back, correctly). The recording already reaches ~0.9 of
full scale, so more PCM gain would only feed the limiter. The remaining
attenuation is the master volume: at 96 the M5 mixer path is ×0.140
(−17.1 dB).

**Change:** default `TTH_SPEAKER_VOLUME` 160 (×0.388, −8.2 dB); runtime key
`v` cycles 96 → 128 → 160 → 192 → 220 → 96 (`tth/SpeakerVolume.h`), capped at
220 (×0.733, −2.7 dB) for this test. The selection is applied when the next
playback opens (never mid-response) and affects loopback and synthetic alike,
since it is the physical speaker level. Each change logs the volume and path
attenuation; `[play] stream open` and `SUMMARY2` report `volume=` and
`pathGainDb=` for the response.

**Unchanged:** PCM gain and limiter, mic magnification, AudioBus, PcmPlayer
queueing, the `playRaw` guard, barge-in.

**Tests (superseded by §16):** `test_speaker_volume` covered the cycle.

## 16. Speaker master volume: 255

Decision: `TTH_SPEAKER_VOLUME` = 255, the maximum M5Unified supports, as both
default and maximum (path gain ×0.984, −0.1 dB). The 96–220 `v` cycle is
removed — with the level fixed at the maximum there is nothing to step
through. The startup report prints `master=255`; every `[play] stream open`
and `SUMMARY2` carries `master=255 pathGainDb=-0.1`. Loopback gain (+9 dB),
limiter, mic, queueing, AudioBus, `playRaw` guard, haptics, bars and
barge-in are unchanged. Remaining risk is analog (speaker rattle, battery
current peaks) and is checked physically.

**Tests:** `test_speaker_volume` (4 cases: 255 is the maximum and passes
the clamp; path gain vs. the mixer formula incl. 255; no digital clipping
at 255 and every lower volume is quieter; squared law); `test_summary`
checks `master=255 pathGainDb=-0.1`.
