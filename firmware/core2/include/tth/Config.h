#pragma once

// TTH Bot Core2 — compile-time configuration.
//
// This is the ONLY file that differs between the `core2-touch` and
// `core2-sw201` PlatformIO environments. The requirement driving that is
// explicit: moving from the capacitive touch button to the external SW201
// must be a configuration change, never a change to conversation, audio or
// face logic. Keeping the selection here (rather than #ifdef-ing inside the
// audio/state-machine code) is what makes that true.

// --- Push-to-talk input selection ------------------------------------------

// Hold-to-talk on M5.BtnB — the CENTER of the three capacitive touch zones in
// the strip below the 320x240 display. Used now; the SW201 is not available
// yet. Note the touch strip is physically outside the display area, so the
// screen stays free for the robot face alone (a Phase 3 requirement).
#define TTH_PTT_TOUCH 1

// Hold-to-talk on a normally-open SW201 momentary push button wired between
// GPIO 33 and GND. Active low with the ESP32's internal pull-up; no external
// resistor required.
#define TTH_PTT_GPIO33 2

// Set by platformio.ini per environment. The default keeps a bare
// `pio run` (or an IDE indexer that misses the build flags) on the input
// that actually exists on the desk today.
#ifndef TTH_PTT_INPUT
#  define TTH_PTT_INPUT TTH_PTT_TOUCH
#endif

#if (TTH_PTT_INPUT != TTH_PTT_TOUCH) && (TTH_PTT_INPUT != TTH_PTT_GPIO33)
#  error "TTH_PTT_INPUT must be TTH_PTT_TOUCH or TTH_PTT_GPIO33"
#endif

// GPIO 33 is Port A's SCL line on the Core2. It is free whenever no Port A
// I2C unit is attached, and — unlike GPIO 34-39 — it supports an internal
// pull-up, which is why it suits a bare normally-open button to GND.
#define TTH_PTT_GPIO_PIN 33

// Software debounce for the physical button, in the required 25-40 ms band.
// The FT6336U touch controller is
// already debounced inside M5Unified, so this only applies to the GPIO input.
#define TTH_PTT_DEBOUNCE_MS 30

// Safety ceiling on a single hold. A stuck button (or a child leaning on the
// device) must not be able to pin the microphone on indefinitely; the turn is
// force-released at this point. Applied to BOTH input implementations so they
// behave identically.
#define TTH_PTT_MAX_HOLD_MS 45000

// --- Haptics ---------------------------------------------------------------

// A short pulse when AI speech begins. Driven by a deadline checked in the
// main loop — never by delay() — so it cannot stall audio or rendering.
#define TTH_VIBRATION_MS 120

// AXP192 LDO3 drive level for the Core2's vibration motor, via
// M5.Power.setVibration(). Firm enough to feel through an enclosure without
// being loud enough to be picked up by the microphone.
#define TTH_VIBRATION_LEVEL 200

// --- Audio -----------------------------------------------------------------
//
// These two rates are NOT interchangeable and must never be crossed: capture
// is 16 kHz, AI playback is 24 kHz. Feeding a 16 kHz buffer to a 24 kHz
// playback call replays it 1.5x too fast. Any path that needs to move audio
// between the two must resample explicitly.
//
// Both values mirror the Flutter app so the two embodiments stay identical:
//   16000 <- lib/features/voice/microphone_service.dart
//   24000 <- lib/features/voice/pcm_audio_player.dart

#define TTH_MIC_SAMPLE_RATE 16000
#define TTH_SPEAKER_SAMPLE_RATE 24000

// --- Serial ----------------------------------------------------------------

#define TTH_SERIAL_BAUD 115200

// --- Robot face palette (Phase 3+; declared here so there is one source) ----
//
// Mirrors lib/core/theme/tth_colors.dart.

#define TTH_COLOR_BACKGROUND 0x060F1Au  // TthColors.faceBackground
#define TTH_COLOR_CYAN 0x4DEFFFu        // TthColors.faceCyan
#define TTH_COLOR_CYAN_DIM 0x2C6E78u    // TthColors.faceCyanDim

// --- Display ---------------------------------------------------------------

#define TTH_DISPLAY_BRIGHTNESS 160

// --- Memory budget ---------------------------------------------------------
//
// The real device reported roughly 4 MB of usable PSRAM in Phase 0, not the
// 8 MB the Core2 datasheet implies. Everything sized against PSRAM -- the face
// sprites (~73 KB) and the audio buffers -- is budgeted against the measured
// figure, and the startup report fails loudly if a board turns up with less.
#define TTH_PSRAM_MIN_BYTES 3500000u

// --- Main loop -------------------------------------------------------------

// How often the health heartbeat is printed.
#define TTH_HEARTBEAT_MS 10000u

// A single main-loop iteration taking longer than this is reported. The loop
// is cooperative: audio servicing and face animation both depend on being
// polled promptly, so a blocking call introduced in a later phase must be
// noticed as a warning here rather than as audio glitching.
#define TTH_LOOP_WARN_MICROS 20000u

// One-off wait in begin() so the USB serial host can attach before the
// startup report is printed. The only intentional wait in the firmware, and
// it happens before the cooperative loop starts.
#define TTH_SERIAL_ATTACH_DELAY_MS 300

// --- Phase 2 diagnostics ---------------------------------------------------
//
// (The Phase 2 Waiting timeout is gone: Waiting now ends because the turn
// source delivered speech.)

// While the button is held, report the elapsed time this often. Without it a
// 45 second maximum-hold test is indistinguishable from a hang on the serial
// monitor.
#define TTH_PTT_HOLD_TICK_MS 5000u

// --- Phase 3 face ----------------------------------------------------------

// Animation frame budget. 40 ms is 25 fps, the rate the mouth animation is
// tuned for. A 300 ms blink still gets around eight frames, which reads as
// smooth.
#define TTH_FACE_FRAME_MS 40u

// How long each face is shown by the 'a' automatic-cycle diagnostic.
#define TTH_FACE_AUTOCYCLE_MS 2500u

// Seed for the blink/pupil-drift generator. Fixed rather than random so that a
// given build always blinks to the same schedule, which makes "did the timing
// change?" answerable during visual testing.
#define TTH_FACE_ANIMATION_SEED 0x54544842u

// --- Phase 4 capture --------------------------------------------------------

// One microphone read. 512 samples at 16 kHz is 32 ms -- small enough that a
// read never dominates a loop iteration, large enough that the DMA is not
// restarted pointlessly often.
#define TTH_CAPTURE_CHUNK_SAMPLES 512u

// The turn buffer must cover a full maximum-length hold. The push-to-talk
// input force-releases at TTH_PTT_MAX_HOLD_MS, so that is the real ceiling:
//
//   16000 samples/s * 2 bytes * 45 s = 1,440,000 bytes
//
// plus one second of margin, so that a late release or a timing edge can never
// reach the capacity and trigger the buffer-full path in normal use.
#define TTH_CAPTURE_MAX_SECONDS (TTH_PTT_MAX_HOLD_MS / 1000u)
#define TTH_CAPTURE_MARGIN_SECONDS 1u

#define TTH_TURN_BUFFER_SAMPLES                     \
  ((uint32_t)TTH_MIC_SAMPLE_RATE *                  \
   (TTH_CAPTURE_MAX_SECONDS + TTH_CAPTURE_MARGIN_SECONDS))
#define TTH_TURN_BUFFER_BYTES (TTH_TURN_BUFFER_SAMPLES * 2u)

// How often the live capture status line is printed. Throttled: at 32 ms per
// chunk an unthrottled line would be 31 lines a second.
#define TTH_CAPTURE_STATUS_MS 500u

// Any single operation that blocks the cooperative loop for longer than this
// is named in the log by TTH_TIME_BLOCK. Deliberately well below
// TTH_LOOP_WARN_MICROS so the culprit is identified before the loop-level
// warning fires.
#define TTH_BLOCK_WARN_MICROS 5000u

// How long the cooperative drain waits for the microphone task to finish with
// the chunk buffers before releasing the peripheral anyway. Spread across loop
// iterations, so this is a deadline, not a stall.
#define TTH_CAPTURE_DRAIN_TIMEOUT_MS 250u

// Most bytes one SerialLog::service() call may write. Bounds a single loop
// iteration regardless of how long the queued line is, or how big the UART
// buffer happens to be. Messages are written in pieces and resume where they
// left off, so nothing depends on a whole line fitting at once.
#define TTH_LOG_WRITE_BUDGET 64u

// --- Phase 5 playback, streaming and the mock turn source -------------------
//
// Still entirely offline: no Wi-Fi, TLS, WebSocket, Gemini or Supabase.

// Which LocalMockTurnSource mode a fresh boot starts in. Switchable at run
// time with the 'm' serial key.
//
//   LOOPBACK   replays the child's recording at its NATIVE 16 kHz. The
//              default, because it is the mode that would make a sample-rate
//              mistake audible.
//   SYNTHETIC  a deterministic 24 kHz voice-like tone -- the assistant's rate.
#define TTH_MOCK_LOOPBACK 1
#define TTH_MOCK_SYNTHETIC 2
#ifndef TTH_MOCK_TURN_MODE
#  define TTH_MOCK_TURN_MODE TTH_MOCK_LOOPBACK
#endif
#if (TTH_MOCK_TURN_MODE != TTH_MOCK_LOOPBACK) && \
    (TTH_MOCK_TURN_MODE != TTH_MOCK_SYNTHETIC)
#  error "TTH_MOCK_TURN_MODE must be TTH_MOCK_LOOPBACK or TTH_MOCK_SYNTHETIC"
#endif

// How long the mock "thinks" after the user turn ends, so the WAITING face is
// actually visible before speech starts.
#define TTH_MOCK_THINK_MS 600u

// Length of a synthetic response: one full quiet / medium / loud cycle of the
// envelope, so all three bar heights are seen.
#define TTH_MOCK_SYNTH_MS 12000u

// Simulated backpressure ('p' key): the mock returns Busy for this long out of
// every period, so the streamer's retry path runs on real hardware.
#define TTH_MOCK_BUSY_WINDOW_MS 150u
#define TTH_MOCK_BUSY_PERIOD_MS 400u

// User audio is streamed in 20 ms frames (320 samples at 16 kHz), matching
// the Flutter app's frame size, and at most this many frames per loop.
#define TTH_STREAM_FRAME_SAMPLES 320u
#define TTH_STREAM_MAX_FRAMES_PER_POLL 2u

// One playback slot. 960 samples is 40 ms at 24 kHz (60 ms at 16 kHz). Three
// slots are preallocated as DMA-capable DRAM at start-up.
#define TTH_PLAYBACK_CHUNK_SAMPLES 960u

// Cooperative deadline for the speaker to go quiet after end-of-stream or a
// cancel. Spread across loop iterations; a deadline, not a stall.
#define TTH_PLAYBACK_DRAIN_TIMEOUT_MS 400u

// Our M5.Speaker virtual channel.
#define TTH_SPEAKER_CHANNEL 0

// M5.Speaker master volume, 0..255 (M5Unified's default is 64).
//
// M5Unified applies it SQUARED (Speaker_Class.cpp: volume = magnification *
// master^2, times channelVolume^2). For 16-bit mono on the Core2 (speaker
// magnification 16, channel volume 255) the digital amplitude factor is
// 16 * V^2 * 255^2 / 2^36:
//
//   V =  64  ->  0.062  (-24.2 dB)     V = 128  ->  0.248  (-12.1 dB)
//   V =  96  ->  0.140  (-17.1 dB)     V = 160  ->  0.388  ( -8.2 dB)
//                                      V = 255  ->  0.984  ( ~0   dB)
//
// So the mixer never amplifies: a full-scale sample cannot clip digitally at
// any volume. The limits above ~160 are ANALOG -- the Core2's small 1 W
// speaker driven by the NS4168 class-D amplifier near full swing rattles and
// distorts, and full-scale bursts draw current peaks from the AXP192 on
// battery. Volume scales synthetic and (later) network speech equally: it is
// the PHYSICAL speaker level.
//
// Set to the MAXIMUM M5Unified supports: 255, path gain x0.984 (-0.1 dB),
// so full scale in is (almost) full scale out. The loopback gain already
// brings the recording to ~0.9 of full scale; this removes the remaining
// attenuation. The analog limits above still apply: listen for rattle on
// loud passages, and watch for resets on battery.
#define TTH_SPEAKER_VOLUME 255
#if (TTH_SPEAKER_VOLUME < 0) || (TTH_SPEAKER_VOLUME > 255)
#  error "TTH_SPEAKER_VOLUME must be 0..255 (M5Unified master volume)"
#endif

// Speech RMS is small -- normal speech sits around 0.05-0.3 of full scale --
// while the cheek bars span 0..1. The playback RMS is multiplied by this
// before smoothing. TUNABLE: confirm visually on the device.
#define TTH_BAR_LEVEL_GAIN 3.0f

// Digital gain for LOCAL LOOPBACK replay only (tth/PcmGain.h). Never applied
// to synthetic 24 kHz speech, and nothing in the player or a future network
// source can apply it: it lives inside LocalMockTurnSource's loopback path.
//
// One gain per turn: this value, reduced only if it would push the turn's
// 99.9th-percentile level above the -3 dBFS soft-limiter knee, and never
// below unity. 0.0 is an exact bypass (the recording, bit for bit).
// Clamped to 0..18 dB. 9.0 dB = x2.82. Cycle 0/6/9/12 dB at run time with 'g'.
#define TTH_LOOPBACK_GAIN_DB 9.0f

// --- Step 6.1: provisioning and Wi-Fi -----------------------------------------
//
// NO credential is ever compiled in: the SSID, password, gateway URL, device
// id and device token are provisioned over USB serial into NVS (namespace
// "tth") and never printed.

// Provisioning lines are a few hundred bytes. The default 256-byte UART receive
// buffer is too tight while a turn slows the loop down.
#define TTH_SERIAL_RX_BUFFER_BYTES 1024

// Most serial bytes consumed per loop iteration, so pasted input can never
// stall the cooperative loop; the rest waits in the UART buffer.
#define TTH_SERIAL_BYTES_PER_LOOP 256u

// A Wi-Fi association that has not completed after this long is abandoned and
// retried with backoff (1, 2, 4, 8, 16, then 30 s, ±20 % jitter).
#define TTH_WIFI_CONNECT_TIMEOUT_MS 15000u

// Push-to-talk refused (not online): two short pulses.
#define TTH_DENIED_PULSE_MS 40
#define TTH_DENIED_GAP_MS 80

// --- Step 6.2: the gateway session ----------------------------------------------
//
// The robot connects to the TTH gateway over TLS (wss://) with its device
// token, sends hello and waits for ready. None of these values is a
// credential: the URL, device id, token and CA are provisioned into NVS.

// hello.fw and the User-Agent ([A-Za-z0-9._-], at most 24 characters).
#define TTH_FIRMWARE_VERSION "core2-6.3"

// hello.credit: the downstream ring's PCM capacity (PHASE6_PLAN §3.3), 4 s of
// 24 kHz speech.
#define TTH_GW_INITIAL_CREDIT 192000u

// The network task (core 0): it owns DNS, TCP, TLS, the upgrade and all I/O.
#define TTH_GW_SOCKET_TIMEOUT_S 10  // TCP connect and each blocking write
#define TTH_GW_TLS_HANDSHAKE_S 15
#define TTH_GW_UPGRADE_TIMEOUT_MS 10000u
#define TTH_GW_TASK_STACK_BYTES 8192
#define TTH_GW_TASK_PRIORITY 2
#define TTH_GW_TASK_CORE 0
#define TTH_GW_EVENT_QUEUE_LENGTH 8
#define TTH_GW_OUTBOUND_QUEUE_LENGTH 4
// Gateway events handled per loop iteration.
#define TTH_GW_EVENTS_PER_LOOP 4u

// GatewaySession (PHASE6_PLAN §8). The connect watchdog covers DNS + TCP (10 s)
// + TLS (15 s) + upgrade (10 s) with margin; ready waits for the gateway's own
// 15 s Gemini setup timeout.
#define TTH_GW_CONNECT_WATCHDOG_MS 45000u
#define TTH_GW_READY_TIMEOUT_MS 25000u
#define TTH_GW_PING_INTERVAL_MS 15000u
#define TTH_GW_PONG_TIMEOUT_MS 10000u
#define TTH_GW_CLOSE_TIMEOUT_MS 5000u
#define TTH_GW_AUTH_RETRY_MS 300000u

// D1: Cloud Run ends a WebSocket request at 60 min. The session reconnects at
// >= 55 min as soon as the conversation is idle, and no turn may start at
// >= 58 min. The diagnostic build (-DTTH_DIAGNOSTIC_BUILD=1) shortens both for
// gate P13; production builds cannot override them (a redefinition is an
// error under -Werror).
#ifdef TTH_DIAGNOSTIC_BUILD
#ifndef TTH_GW_PROACTIVE_RECONNECT_MS
#define TTH_GW_PROACTIVE_RECONNECT_MS 120000u  // 2 min
#endif
#ifndef TTH_GW_TURN_GUARD_MS
#define TTH_GW_TURN_GUARD_MS 180000u  // 3 min
#endif
#else
#define TTH_GW_PROACTIVE_RECONNECT_MS 3300000u  // 55 min
#define TTH_GW_TURN_GUARD_MS 3480000u           // 58 min
#endif

// --- Step 6.2: memory placement (PHASE6_PLAN §7) -----------------------------
//
// FACE SPRITES IN PSRAM, decided at compile time. The first physical Step 6.2
// run failed the memory gate with the sprites in internal DRAM: during
// playback, current internal free fell to 28 472 B (< 32 KB) with a largest
// block of 25 588 B. The two sprites (81 112 B) therefore move to PSRAM. The
// renderer allocates exactly where these say and refuses to render if the
// buffer lands anywhere else -- there is no runtime fallback. The price is
// rendering time (PSRAM bandwidth), measured and reported, never hidden by
// raising TTH_LOOP_WARN_MICROS.
#define TTH_FACE_EYES_IN_PSRAM 1
#define TTH_FACE_LOWER_FACE_IN_PSRAM 1

#if (TTH_FACE_EYES_IN_PSRAM != 0 && TTH_FACE_EYES_IN_PSRAM != 1) || \
    (TTH_FACE_LOWER_FACE_IN_PSRAM != 0 && TTH_FACE_LOWER_FACE_IN_PSRAM != 1)
#error "sprite placement flags must be 0 (internal DRAM) or 1 (PSRAM)"
#endif

// A new internal-heap low-water mark this far below the last reported one is
// logged with the loop stage that first saw it.
#define TTH_MEM_LOWWATER_REPORT_BYTES 2048u

// --- Step 6.3: gateway turns ------------------------------------------------------
//
// PRODUCTION ALWAYS USES THE GATEWAY TURN SOURCE. The local mock exists only
// in a diagnostic build (-DTTH_DIAGNOSTIC_BUILD=1); the production image has
// no key that switches to it.

// One downstream frame: at most 1920 B = 960 samples = 40 ms at 24 kHz, exactly
// one PcmPlayer slot. Credit is returned in batches of two such frames.
#define TTH_GW_DOWN_FRAME_BYTES 1920u
#define TTH_GW_CREDIT_BATCH_BYTES (2u * TTH_GW_DOWN_FRAME_BYTES)  // 3840 B

#if TTH_GW_DOWN_FRAME_BYTES != TTH_PLAYBACK_CHUNK_SAMPLES * 2u
#error "a downstream frame must be exactly one playback slot"
#endif

// The downstream ring, allocated once in PSRAM with no DRAM fallback: the
// credited PCM plus a 12 B header per frame for up to TTH_GW_RING_MAX_FRAMES
// frames (an average frame of >= 24 B). A frame that does not fit is a
// protocol violation, like one over credit.
#define TTH_GW_RING_PCM_BYTES TTH_GW_INITIAL_CREDIT
#define TTH_GW_RING_HEADER_BYTES 12u
#define TTH_GW_RING_MAX_FRAMES 8192u
#define TTH_GW_RING_BYTES \
  (TTH_GW_RING_PCM_BYTES + TTH_GW_RING_MAX_FRAMES * TTH_GW_RING_HEADER_BYTES)  // 290 304 B

// Outbound items the NetSender writes per service pass before it reads again.
#define TTH_GW_SEND_ITEMS_PER_SERVICE 8u

// No valid response this long after turn_end was fully written: the turn
// fails (ResponseTimeout); the connection stays up. Other turn, inactivity and
// reconnect timeouts are Step 6.4.
#define TTH_GW_FIRST_RESPONSE_TIMEOUT_MS 10000u

// A failed gateway turn shows ERROR at least this long, then returns to the
// rest state for the link (Ready, or Sleeping while offline). A press retries
// at once.
#define TTH_TURN_ERROR_HOLD_MS 3000u

#if TTH_GW_PROACTIVE_RECONNECT_MS >= TTH_GW_TURN_GUARD_MS || TTH_GW_TURN_GUARD_MS >= 3600000u
#error "the proactive reconnect must precede the turn guard, and both must precede 60 min"
#endif
