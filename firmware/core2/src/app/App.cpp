#include "app/App.h"

#include <Arduino.h>
#include <M5Unified.h>

#include "diag/BlockStats.h"
#include "diag/BlockTimer.h"
#include "diag/StartupReport.h"
#include "input/PushToTalkInputFactory.h"
#include "tth/Config.h"
#include "tth/PcmGain.h"
#include "tth/PlaybackSummary.h"
#include "tth/SpeakerVolume.h"
#include "tth/StageTimer.h"

namespace tth {

namespace {

// Feeds portable stage timings (CaptureController::start, BargeIn) into the
// same aggregate table the TTH_TIME_BLOCK macros use, so the whole breakdown
// -- portable stages and device calls alike -- appears on one [blocks] line.
class AppStageTimer : public IStageTimer {
 public:
  uint32_t nowMicros() const override { return micros(); }
  void report(const char* stage, uint32_t elapsedMicros) override {
    diag::BlockStats::record(stage, elapsedMicros, millis());
  }
};

AppStageTimer g_stageTimer;

// Provisioning replies go through the shared log queue as critical lines: they
// answer a deliberate command and must not be crowded out by routine output.
class QueueLineSink : public ILineSink {
 public:
  explicit QueueLineSink(LogQueue& queue) : _queue(queue) {}
  void line(const char* text) override { _queue.pushCritical(text); }

 private:
  LogQueue& _queue;
};

// The boot report, before the cooperative loop starts: straight to Serial.
class SerialLineSink : public ILineSink {
 public:
  void line(const char* text) override { Serial.println(text); }
};

#ifdef TTH_DIAGNOSTIC_BUILD
MockConfig makeMockConfig() {
  MockConfig config;
  config.captureFormat = monoS16(TTH_MIC_SAMPLE_RATE);
  config.assistantFormat = monoS16(TTH_SPEAKER_SAMPLE_RATE);
  config.thinkMs = TTH_MOCK_THINK_MS;
  config.synthDurationMs = TTH_MOCK_SYNTH_MS;
  config.busyWindowMs = TTH_MOCK_BUSY_WINDOW_MS;
  config.busyPeriodMs = TTH_MOCK_BUSY_PERIOD_MS;
  config.loopbackGainDb = TTH_LOOPBACK_GAIN_DB;
  return config;
}
#endif

// The wire limits and Config.h must agree, or a frame would not fit a slot.
static_assert(TTH_GW_DOWN_FRAME_BYTES == wire::kMaxDownPcmBytes,
              "a downstream frame is exactly the protocol maximum");
static_assert(TTH_STREAM_FRAME_SAMPLES * 2u <= wire::kMaxUpPcmBytes,
              "an up frame must fit the protocol maximum");
static_assert(TTH_GW_RING_HEADER_BYTES == DownstreamRing::kHeaderBytes,
              "ring header size");

GatewayTurnConfig makeGatewayTurnConfig() {
  GatewayTurnConfig config;
  config.captureFormat = monoS16(TTH_MIC_SAMPLE_RATE);
  config.responseFormat = monoS16(TTH_SPEAKER_SAMPLE_RATE);
  config.firstResponseTimeoutMs = TTH_GW_FIRST_RESPONSE_TIMEOUT_MS;
  config.maxUpSamples = TTH_STREAM_FRAME_SAMPLES;
  return config;
}

// M5Unified mixer attenuation (dB) at `master`, with this build's speaker
// magnification and channel volume.
double pathGainDbAt(uint8_t master) {
  return speakerPathGainDb(
      M5.Speaker.config().magnification, master,
      M5.Speaker.getChannelVolume(TTH_SPEAKER_CHANNEL));
}

// GatewaySession's timings, from include/tth/Config.h.
SessionTimings gatewayTimings() {
  SessionTimings t;
  t.connectTimeoutMs = TTH_GW_CONNECT_WATCHDOG_MS;
  t.readyTimeoutMs = TTH_GW_READY_TIMEOUT_MS;
  t.pingIntervalMs = TTH_GW_PING_INTERVAL_MS;
  t.pongTimeoutMs = TTH_GW_PONG_TIMEOUT_MS;
  t.closeTimeoutMs = TTH_GW_CLOSE_TIMEOUT_MS;
  t.authRetryMs = TTH_GW_AUTH_RETRY_MS;
  t.proactiveReconnectMs = TTH_GW_PROACTIVE_RECONNECT_MS;
  t.turnGuardMs = TTH_GW_TURN_GUARD_MS;
  return t;
}

const char* closeCauseName(GatewayClient::CloseCause cause) {
  switch (cause) {
    case GatewayClient::CloseCause::Local:
      return "closed by the robot";
    case GatewayClient::CloseCause::Peer:
      return "closed by the gateway";
    case GatewayClient::CloseCause::Transport:
      return "connection lost";
    case GatewayClient::CloseCause::WriteFailed:
      return "write failed";
    case GatewayClient::CloseCause::Protocol:
      return "protocol error";
  }
  return "invalid";
}

bool isHttpFailure(ConnectFailure failure) {
  return failure == ConnectFailure::Unauthorized || failure == ConnectFailure::Throttled ||
         failure == ConnectFailure::ServerUnavailable ||
         failure == ConnectFailure::ServerError || failure == ConnectFailure::Rejected;
}

}  // namespace

App::App()
    : _audioBus(_audioDevice),
      _capture(_audioBus, _microphone, TTH_PTT_MAX_HOLD_MS,
               TTH_CAPTURE_DRAIN_TIMEOUT_MS),
      _turnStorage(nullptr),
      _chunkStorage(nullptr),
      _micChunk{nullptr, nullptr},
      _lastCaptureStatusMs(0),
      _log(diag::log()),
      _maxDrainPollMicros(0),
      _heartbeatStage(0),
      _ptt(pushToTalkInput()),
      _captureFormat(monoS16(TTH_MIC_SAMPLE_RATE)),
      _speakerOutput(TTH_SPEAKER_CHANNEL),
      _player(_audioBus, _speakerOutput, TTH_PLAYBACK_DRAIN_TIMEOUT_MS),
      _playbackSlot{nullptr, nullptr, nullptr},
      _streamer(TTH_STREAM_FRAME_SAMPLES, TTH_STREAM_MAX_FRAMES_PER_POLL),
#ifdef TTH_DIAGNOSTIC_BUILD
      _synthScratch(nullptr),
      _mockTurn(_capture.turn(), makeMockConfig()),
      _useMock(false),
#endif
      _bargeIn(*this),
      _acceptPlayback(false),
      _speechStartPending(false),
      _pendingSpeechFormat(monoS16(0)),
      _speechStartMs(0),
      _maxLoopSpeakingMicros(0),
      _streamVolume(clampSpeakerVolume(TTH_SPEAKER_VOLUME)),
      _faceAnimator(TTH_FACE_ANIMATION_SEED),
      _fixedLevelIndex(kFixedLevelCount),
      _faceOverride(TTH_FACE_AUTOCYCLE_MS),
      _shownFace(FaceState::Ready),
      _faceEnteredMs(0),
      _lastFaceFrameMs(0),
      _faceTransitionPending(true),
      _bootMs(0),
      _lastHeartbeatMs(0),
      _maxLoopSteadyMicros(0),
      _maxLoopTransitionMicros(0),
      _transitionIteration(false),
      _maxSerialWriteMicros(0),
      _loopCount(0),
      _lastHoldTickMs(0),
      _configStore(_nvs),
      _provisioning(_configStore, _random),
      _trustStore(_nvs),
      _caActive(nullptr),
      _caScratchA(nullptr),
      _caScratchB(nullptr),
      _caStaging(nullptr),
      _session(gatewayTimings(), 1),
      _gatewayGeneration(0),
      _staleGatewayEvents(0),
      _connectivity(Connectivity::Offline),
      _m2Reported(false),
      _lowWater(TTH_MEM_LOWWATER_REPORT_BYTES),
      _gatewayTurn(_gateway, _gateway.ring(), _gateway.credit(), makeGatewayTurnConfig()),
      _turnSource(&_gatewayTurn),
      _reportedZeroStalls(0),
      _lastZeroStallReportMs(0),
      _startFailureTransient(false),
      _activitySelector(TTH_ACTIVITY_MENU_TIMEOUT_MS, TTH_ACTIVITY_SELECT_TIMEOUT_MS,
                        TTH_ACTIVITY_FAIL_DISPLAY_MS),
      _activityPreference(_nvs),
      _activityMenuDirty(false) {
  _savedActivity[0] = '\0';
}

void App::begin() {
  // Before M5.begin() starts the UART: provisioning lines need more than the
  // default 256-byte receive buffer.
  Serial.setRxBufferSize(TTH_SERIAL_RX_BUFFER_BYTES);

  auto cfg = M5.config();
  cfg.serial_baudrate = TTH_SERIAL_BAUD;
  cfg.clear_display = true;
  cfg.internal_mic = true;
  cfg.internal_spk = true;
  M5.begin(cfg);

  // Orientation unchanged from Phase 1/2: landscape, 320x240.
  M5.Display.setRotation(1);
  M5.Display.setBrightness(TTH_DISPLAY_BRIGHTNESS);

  // Give the USB serial host a moment to attach so the report is not printed
  // into a void. This is the only intentional wait in the firmware, and it
  // happens before the cooperative loop starts.
  M5.delay(TTH_SERIAL_ATTACH_DELAY_MS);

  diag::printStartupReport(_ptt.name());

  _faceGeometry =
      FaceGeometry::forScreen(M5.Display.width(), M5.Display.height());
  diag::printFaceGeometry(_faceGeometry);

  if (!_faceRenderer.begin(_faceGeometry)) {
    Serial.println(F("[face] *** renderer failed to start; face disabled ***"));
  }

  _audioDevice.configure();
  // Start from a known, unowned bus. Because AudioBus tracks ownership, this
  // issues no hardware calls at all -- which is precisely what stops the
  // "I2S port N has not installed" error the Phase 0 diagnostic produced by
  // calling end() unconditionally.
  _audioBus.releaseAll();
  Serial.printf("[audio] bus owner at boot: %s (no hardware calls made)\r\n",
                toString(_audioBus.owner()));

  // THE SIGNAL PATH as actually configured, so loudness can be reasoned
  // about from the log instead of guessed.
  {
    const auto mic = M5.Mic.config();
    const auto spk = M5.Speaker.config();
    const double master = M5.Speaker.getVolume();
    const double channel = M5.Speaker.getChannelVolume(TTH_SPEAKER_CHANNEL);
    // M5Unified mixer, 16-bit mono (Speaker_Class.cpp): out = in *
    // magnification * master^2 * channel^2 / 2^36. Volume is SQUARED.
    const double factor = static_cast<double>(spk.magnification) * master *
                          master * channel * channel / 68719476736.0;
    const double factorDb = (factor > 0.0) ? 20.0 * log10(factor) : -120.0;
    Serial.printf("[audio] mic: %lu Hz, magnification=%u, over_sampling=%u, "
                  "noise_filter=%u; capture adds DC removal only\r\n",
                  static_cast<unsigned long>(mic.sample_rate),
                  static_cast<unsigned>(mic.magnification),
                  static_cast<unsigned>(mic.over_sampling),
                  static_cast<unsigned>(mic.noise_filter_level));
    Serial.printf("[audio] speaker: %lu Hz out, magnification=%u, master=%u, "
                  "channel=%u -> path gain %.3f (%.1f dB of full scale)\r\n",
                  static_cast<unsigned long>(spk.sample_rate),
                  static_cast<unsigned>(spk.magnification),
                  static_cast<unsigned>(master), static_cast<unsigned>(channel),
                  factor, factorDb);
#ifdef TTH_DIAGNOSTIC_BUILD
    Serial.printf("[audio] loopback gain: %+.1f dB configured (diagnostic mock "
                  "loopback only; gateway audio untouched)\r\n",
                  static_cast<double>(_mockTurn.loopbackGainDb()));
#endif
  }

  if (!allocateAudioBuffers()) {
    Serial.println(F("[audio] *** buffers unavailable; audio disabled ***"));
  }

  // Printed after every buffer is allocated, so it is the real steady-state
  // baseline networking will later be measured against.
  diag::printMemoryReport(diag::readMemoryReport(), _faceRenderer.eyesRegion(),
                          _faceRenderer.lowerFaceRegion(),
                          _faceRenderer.spriteBytes(), TTH_TURN_BUFFER_BYTES);

  // --- Step 6.1: stored configuration, then Wi-Fi ---------------------------
  Serial.println(F("[mem] M0 = the memory report above (all buffers allocated, "
                   "Wi-Fi not started)"));
  {
    SerialLineSink boot;
    if (!_nvs.begin()) {
      Serial.println(F("[config] *** NVS namespace \"tth\" could not be opened; "
                       "running unprovisioned ***"));
    }
    const LoadReport loaded = _configStore.load();
    Serial.printf("[config] slot A: %s (generation %lu) | slot B: %s "
                  "(generation %lu)\r\n",
                  config::toString(loaded.a.status),
                  static_cast<unsigned long>(loaded.a.generation),
                  config::toString(loaded.b.status),
                  static_cast<unsigned long>(loaded.b.generation));
    if (_configStore.hasConfig()) {
      Serial.printf("[config] using slot %c, generation %lu\r\n", loaded.chosen,
                    static_cast<unsigned long>(loaded.generation));
    } else {
      Serial.println(F("[config] no configuration stored: provision over USB "
                       "serial (!prov help)"));
    }
    // Presence and safe metadata only; never the SSID, password or token.
    reportConfig("stored", _configStore.active(), _configStore.hasConfig(), boot);

    // --- Step 6.2: the gateway CA (public) ---------------------------------
    if (allocateTrustAnchorBuffers()) {
      const TrustLoadReport trust = _trustStore.load();
      Serial.printf("[config] ca slot A: %s (generation %lu) | slot B: %s "
                    "(generation %lu)\r\n",
                    config::toString(trust.a),
                    static_cast<unsigned long>(trust.generationA),
                    config::toString(trust.b),
                    static_cast<unsigned long>(trust.generationB));
      _provisioning.attachTrustAnchors(_trustStore, _caStaging,
                                       trust::kMaxPemBytes + 1, &_certificateCheck);
    } else {
      Serial.println(F("[config] *** CA buffers unavailable: gateway disabled ***"));
    }
    reportTrustAnchor("stored", _trustStore, &_certificateCheck, boot);

    // The saved on-device activity selection (an id only), sent in hello.
    if (_activityPreference.load(_savedActivity, sizeof(_savedActivity))) {
      Serial.printf("[activity] saved selection: %s\r\n", _savedActivity);
    } else {
      Serial.println(F("[activity] saved selection: none (the gateway's configured activity)"));
    }
  }
  _wifi.begin();
  _wifi.apply(_configStore.hasConfig() ? &_configStore.active() : nullptr,
              millis());

  // --- Step 6.2: the gateway session -----------------------------------------
  _session.reseed(esp_random());
  if (_gateway.begin()) {
    Serial.printf("[gw] network task on core %d (priority %d, stack %d B); TLS "
                  "with the pinned CA and host name, no insecure mode\r\n",
                  TTH_GW_TASK_CORE, TTH_GW_TASK_PRIORITY, TTH_GW_TASK_STACK_BYTES);
    // Step 6.3 allocations, with where they actually landed.
    const uintptr_t ring = reinterpret_cast<uintptr_t>(_gateway.ringStorage());
    const uintptr_t queue = reinterpret_cast<uintptr_t>(_gateway.queueStorage());
    Serial.printf("[gw] downstream ring %lu B (credit %lu B + %lu x %lu B frame headers) "
                  "at 0x%08lx %s | outbound queue %lu B at 0x%08lx %s\r\n",
                  static_cast<unsigned long>(TTH_GW_RING_BYTES),
                  static_cast<unsigned long>(TTH_GW_RING_PCM_BYTES),
                  static_cast<unsigned long>(TTH_GW_RING_MAX_FRAMES),
                  static_cast<unsigned long>(TTH_GW_RING_HEADER_BYTES),
                  static_cast<unsigned long>(ring), memoryRegionName(ring),
                  static_cast<unsigned long>(sizeof(OutboundQueue)),
                  static_cast<unsigned long>(queue), memoryRegionName(queue));
    Serial.printf("[gw] down frames <= %u B (%u samples = 40 ms at 24 kHz = one playback "
                  "slot); credit returned in %u B batches; first-response timeout %lu ms\r\n",
                  static_cast<unsigned>(TTH_GW_DOWN_FRAME_BYTES),
                  static_cast<unsigned>(TTH_PLAYBACK_CHUNK_SAMPLES),
                  static_cast<unsigned>(TTH_GW_CREDIT_BATCH_BYTES),
                  static_cast<unsigned long>(TTH_GW_FIRST_RESPONSE_TIMEOUT_MS));
  } else {
    Serial.println(F("[gw] *** network task could not start: gateway disabled ***"));
  }
  // Always printed, so the boot report proves which timings are flashed: a
  // diagnostic build (gate P13) must never be mistaken for production.
  Serial.printf("[gw] timings: %s build - proactive reconnect %lu s, turn guard "
                "%lu s, ping %lu s, pong %lu s, auth retry %lu s\r\n",
#ifdef TTH_DIAGNOSTIC_BUILD
                "DIAGNOSTIC",
#else
                "production",
#endif
                static_cast<unsigned long>(TTH_GW_PROACTIVE_RECONNECT_MS / 1000u),
                static_cast<unsigned long>(TTH_GW_TURN_GUARD_MS / 1000u),
                static_cast<unsigned long>(TTH_GW_PING_INTERVAL_MS / 1000u),
                static_cast<unsigned long>(TTH_GW_PONG_TIMEOUT_MS / 1000u),
                static_cast<unsigned long>(TTH_GW_AUTH_RETRY_MS / 1000u));
  applyGatewayTarget(millis());

  // Alert lines from the block timers go through the same bounded queue.
  diag::BlockStats::setQueue(&_log.queue());
  // Stage timings from portable code -- capture start and each barge-in step
  // -- land in the same [blocks] breakdown.
  _capture.setStageTimer(&g_stageTimer);
  _bargeIn.setStageTimer(&g_stageTimer);

#ifndef TTH_DIAGNOSTIC_BUILD
  Serial.println(F("[turn] source: gateway (tth.v1); this image has no local mock"));
#else
  Serial.println(F("[turn] source: gateway (tth.v1) at boot; DIAGNOSTIC image includes the local mock"));
  _mockTurn.setMode(TTH_MOCK_TURN_MODE == TTH_MOCK_SYNTHETIC
                        ? MockMode::Synthetic
                        : MockMode::Loopback);
  Serial.printf(
      "[turn] DIAGNOSTIC build: 't' switches to the local mock, mode=%s (16 kHz "
      "loopback plays at 16 kHz; synthetic at %u Hz), think %u ms\r\n",
      toString(_mockTurn.mode()),
      static_cast<unsigned>(TTH_SPEAKER_SAMPLE_RATE),
      static_cast<unsigned>(TTH_MOCK_THINK_MS));
#endif
  Serial.printf(
      "[stream] %u-sample frames (%u ms), up to %u per loop, read from the "
      "turn buffer under a lease\r\n",
      static_cast<unsigned>(TTH_STREAM_FRAME_SAMPLES),
      static_cast<unsigned>((TTH_STREAM_FRAME_SAMPLES * 1000u) /
                            TTH_MIC_SAMPLE_RATE),
      static_cast<unsigned>(TTH_STREAM_MAX_FRAMES_PER_POLL));

  _ptt.begin();

  _bootMs = millis();
  _lastHeartbeatMs = _bootMs;
  _lastHoldTickMs = _bootMs;
  _faceEnteredMs = _bootMs;
  _lastFaceFrameMs = _bootMs;

  // Leaving Booting. Reported through the same path as every later
  // transition, so the serial log has no special cases.
  // The link decides where start-up lands: Ready only when online, otherwise
  // Connecting or Disconnected -- both shown with the Sleeping face.
  _connectivity = computeConnectivity();
  _machine.setConnectivity(_connectivity);
  const ConversationState previous = _machine.state();
  logStateIfChanged(_machine.markReady(), previous);

  _shownFace = faceStateFor(_machine.state());
  _faceAnimator.setState(_shownFace, _bootMs);

  // Low-water attribution starts from the settled boot figure.
  _lowWater.reset(static_cast<uint32_t>(
      heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));

  Serial.println(F("[app] ready - cooperative loop running"));
  printFaceCommandHelp();
}

void App::tick() {
  const uint32_t tickStartMicros = micros();

  // Drives buttons, touch and power management. Everything else in the loop
  // observes the state it latches, so it must come first.
  M5.update();

  const uint32_t nowMs = millis();

  // Internal-heap low-water checkpoints (Step 6.2 memory investigation). The
  // first one also covers everything that ran while the loop yielded: the
  // Wi-Fi, lwIP, TLS and audio tasks, and the previous iteration's log output.
  checkLowWater("yield (wifi/tls/audio tasks), M5.update", nowMs);

  _ptt.poll(nowMs);
  // The link first, so a press in this same iteration sees current
  // connectivity.
  serviceNetwork(nowMs);
  checkLowWater("network service", nowMs);
  // The activity menu reads the touch zones before push-to-talk does, so a
  // centre press while the menu is shown is the menu's and never a capture.
  serviceActivityMenu(nowMs);
  servicePushToTalk(nowMs);
  // Capture first, so the streamer sees this iteration's committed audio.
  serviceCapture(nowMs);
  serviceStream(nowMs);
  checkLowWater("ptt, capture, stream", nowMs);
  // Then the response side: events and audio pulled from the source, fed to
  // the player, the player serviced, and any barge-in advanced.
  serviceTurnSource(nowMs);
  servicePlayback(nowMs);
  serviceBargeIn(nowMs);
  serviceErrorHold(nowMs);
  _haptics.poll(nowMs);
  checkLowWater("turn source, playback, barge-in, haptics", nowMs);
  serviceSerialCommands(nowMs);
  {
    TTH_TIME_BLOCK("face render");
    serviceFace(nowMs);
  }
  checkLowWater("serial commands, face render", nowMs);

  serviceHeartbeat(nowMs);
  checkLowWater("heartbeat", nowMs);

  // Application work ends here. Everything after this is optional diagnostic
  // output, and is measured separately so it cannot disguise -- or be mistaken
  // for -- real application blocking.
  const uint32_t workElapsed = micros() - tickStartMicros;
  ++_loopCount;

  // A turn starting or ending costs one deterministic audio-frame priming
  // stall, which says nothing about the budget the rest of the loop lives in.
  // Keeping the two apart is what stops a known 46 ms transition hiding a
  // creeping steady-state regression.
  if (_transitionIteration) {
    if (workElapsed > _maxLoopTransitionMicros) {
      _maxLoopTransitionMicros = workElapsed;
    }
    _transitionIteration = false;
  } else {
    if (workElapsed > _maxLoopSteadyMicros) _maxLoopSteadyMicros = workElapsed;
    // Steady state WHILE SPEAKING, separately: playback is the new load this
    // phase adds, and this is the figure that says whether it fits.
    if (_machine.state() == ConversationState::Speaking &&
        workElapsed > _maxLoopSpeakingMicros) {
      _maxLoopSpeakingMicros = workElapsed;
    }
  }

  const uint32_t serialElapsed = _log.service();
  if (serialElapsed > _maxSerialWriteMicros) {
    _maxSerialWriteMicros = serialElapsed;
  }

  // Yield to the scheduler without blocking: M5.delay() pumps M5Unified's
  // internals while letting FreeRTOS run its background tasks. Never use
  // Arduino delay() here.
  M5.delay(1);
}

void App::servicePushToTalk(uint32_t nowMs) {
  if (_activitySelector.isOpen()) {
    // The centre zone belongs to the menu: push-to-talk edges are swallowed,
    // so no capture can start while it is shown (or while a selection is
    // pending).
    if (_ptt.consumePress()) _log.printf("[ptt] press ignored: activity menu open");
    _ptt.consumeRelease();
    return;
  }
  if (_ptt.consumePress()) {
    _log.printf("[ptt] PRESS");
    _lastHoldTickMs = nowMs;
    // Any real press hands the face back to the state machine, so a forgotten
    // diagnostic override can never mask actual behaviour.
    if (_faceOverride.isActive()) {
      _faceOverride.clear();
      _fixedLevelIndex = kFixedLevelCount;
      _log.printf("[face] override cleared by push-to-talk");
    }
    handlePress(nowMs);
  }

  // A long hold would otherwise look like a hang on the serial monitor; this
  // makes the 45 second maximum-hold test observable while it is happening.
  if (_ptt.isHeld() && (nowMs - _lastHoldTickMs) >= TTH_PTT_HOLD_TICK_MS) {
    _lastHoldTickMs = nowMs;
    _log.printf("[ptt] holding %lu ms",
                static_cast<unsigned long>(_ptt.heldForMs(nowMs)));
  }

  if (_ptt.consumeRelease()) {
    // Taken from the input, which captured it at the instant the release edge
    // was generated -- heldForMs() is already 0 by now.
    const unsigned long heldMs = static_cast<unsigned long>(_ptt.lastHoldMs());
    if (_ptt.lastReleaseWasForced()) {
      _log.printf("[ptt] FORCED RELEASE after %lu ms (maximum hold %lu ms)",
                  heldMs, static_cast<unsigned long>(TTH_PTT_MAX_HOLD_MS));
    } else {
      _log.printf("[ptt] RELEASE after %lu ms", heldMs);
    }
    finishTurn(nowMs, _ptt.lastReleaseWasForced()
                          ? CaptureStopReason::MaxHold
                          : CaptureStopReason::ButtonRelease);
  }
}

void App::handlePress(uint32_t nowMs) {
  // Not online: no LISTENING, and two short pulses so a child can feel that
  // the robot is not ready yet (PHASE6_PLAN §9). Checked before anything else,
  // so nothing is started or stopped.
  // Online means Wi-Fi up AND the gateway session ready (Step 6.2). No turn
  // starts once the session is due for its proactive reconnect (D1).
  if (_connectivity != Connectivity::Online || !_session.mayStartTurn(nowMs)) {
    if (_connectivity == Connectivity::Online) {
      _log.printf("[ptt] press refused: gateway session due for reconnect (age %lu s)",
                  static_cast<unsigned long>(_session.sessionAgeMs(nowMs) / 1000u));
    } else {
      _log.printf("[ptt] press refused: not online (wifi %s, gateway %s)",
                  toString(_wifi.state()), toString(_session.state()));
    }
    if (_haptics.denied(nowMs)) {
      _log.printf("[haptics] refused pattern (2 short pulses)");
    }
    return;
  }

  if (_bargeIn.isActive()) {
    _log.printf("[ptt] press ignored: barge-in already in progress");
    return;
  }

  // The speaker still owns the hardware -- either the robot is talking, or a
  // failed response is still draining. The microphone may not start until the
  // speaker is quiet and released, so this goes through the ordered barge-in
  // sequence rather than straight to capture.
  if (!_player.isIdle()) {
    markTransitionIteration();
    _log.printf("[barge] press while speaker active (player %s): stopping "
                "playback first",
                toString(_player.state()));
    _bargeIn.begin(nowMs);
    return;
  }

  const ConversationState state = _machine.state();
  if (state == ConversationState::Listening ||
      state == ConversationState::Waiting) {
    // The previous turn is still being handed over or answered.
    _log.printf("[ptt] press ignored in %s", toString(state));
    return;
  }

  beginTurn(nowMs);
}

bool App::allocateAudioBuffers() {
  // ONCE, at start-up. Nothing allocates during a turn or a response: a malloc
  // stall mid-recording would drop audio the child has already spoken, and a
  // failure there would be unrecoverable.
  //
  // MEMORY CAPS MATTER HERE -- this is where the Phase 4 crash came from.
  //
  // MALLOC_CAP_INTERNAL only means "not SPIRAM". It can, and did, return IRAM,
  // which is word-access-only: M5Unified's microphone task stores int16
  // samples straight into the buffer and the first sample raised a
  // LoadStoreError. Every buffer below therefore asks for what it actually
  // needs and is verified against tth/MemorySafety.h before use.
  const size_t chunkBytes = TTH_CAPTURE_CHUNK_SAMPLES * sizeof(int16_t);
  const size_t playbackBytes = TTH_PLAYBACK_CHUNK_SAMPLES * sizeof(int16_t);

  // The turn buffer: 1.4 MB will not fit the internal heap, and PSRAM is byte
  // addressable, which is all it needs -- no DMA touches it.
  _turnStorage = static_cast<int16_t*>(
      heap_caps_malloc(TTH_TURN_BUFFER_BYTES, MALLOC_CAP_SPIRAM));
  if (_turnStorage == nullptr) {
    Serial.printf("[capture] PSRAM turn buffer (%lu bytes) FAILED\r\n",
                  static_cast<unsigned long>(TTH_TURN_BUFFER_BYTES));
    return false;
  }

  // The two microphone destinations: written by the I2S task, so they must be
  // DMA-capable internal DRAM. MALLOC_CAP_DMA implies internal + 8-bit
  // addressable, which is exactly the requirement.
  for (int i = 0; i < 2; ++i) {
    _micChunk[i] =
        static_cast<int16_t*>(heap_caps_malloc(chunkBytes, MALLOC_CAP_DMA));
    if (_micChunk[i] == nullptr) {
      Serial.printf("[capture] DMA chunk buffer %d FAILED\r\n", i);
      return false;
    }
  }

  // The controller's scratch chunk: only our own code touches it, but we do
  // 16-bit accesses, so it still must not land in IRAM.
  _chunkStorage = static_cast<int16_t*>(
      heap_caps_malloc(chunkBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (_chunkStorage == nullptr) {
    Serial.println(F("[capture] scratch chunk buffer FAILED"));
    return false;
  }

  // The three playback slots: READ by the speaker task from its own task, for
  // as long as the speaker holds them. Same requirement as the mic chunks.
  for (uint32_t i = 0; i < PcmPlayer::kSlots; ++i) {
    _playbackSlot[i] =
        static_cast<int16_t*>(heap_caps_malloc(playbackBytes, MALLOC_CAP_DMA));
    if (_playbackSlot[i] == nullptr) {
      Serial.printf("[play] DMA playback slot %lu FAILED\r\n",
                    static_cast<unsigned long>(i));
      return false;
    }
  }

#ifdef TTH_DIAGNOSTIC_BUILD
  // Synthetic speech scratch (diagnostic mock only): only our code reads or
  // writes it (the player copies out of it), so byte-addressable internal DRAM
  // is enough.
  _synthScratch = static_cast<int16_t*>(
      heap_caps_malloc(playbackBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (_synthScratch == nullptr) {
    Serial.println(F("[play] synthetic scratch buffer FAILED"));
    return false;
  }
#endif

  // Verify every buffer really is where it needs to be. A wrong region is a
  // guaranteed panic later, so refuse to start instead.
  struct Check {
    const char* name;
    const void* pointer;
    bool needsDma;
  };
  const Check checks[] = {
      {"turn (PSRAM)", _turnStorage, false},
      {"mic chunk A", _micChunk[0], true},
      {"mic chunk B", _micChunk[1], true},
      {"scratch chunk", _chunkStorage, false},
      {"play slot A", _playbackSlot[0], true},
      {"play slot B", _playbackSlot[1], true},
      {"play slot C", _playbackSlot[2], true},
#ifdef TTH_DIAGNOSTIC_BUILD
      {"synth scratch", _synthScratch, false},
#endif
  };

  bool safe = true;
  for (const Check& check : checks) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(check.pointer);
    const bool ok = check.needsDma ? isDmaCapable(address)
                                   : isByteAddressable(address);
    Serial.printf("[audio] %-14s 0x%08lx %-7s %s\r\n", check.name,
                  static_cast<unsigned long>(address),
                  memoryRegionName(address), ok ? "ok" : "*** WRONG REGION ***");
    if (!ok) safe = false;
  }
  if (!safe) {
    Serial.println(F("[audio] refusing to start: a buffer is in a region "
                     "that cannot hold 16-bit PCM safely"));
    return false;
  }

  _capture.begin(_turnStorage, TTH_TURN_BUFFER_SAMPLES, _chunkStorage,
                 TTH_CAPTURE_CHUNK_SAMPLES);
  _microphone.begin(_micChunk[0], _micChunk[1], TTH_CAPTURE_CHUNK_SAMPLES,
                    TTH_MIC_SAMPLE_RATE);
  _player.begin(_playbackSlot[0], _playbackSlot[1], _playbackSlot[2],
                TTH_PLAYBACK_CHUNK_SAMPLES);
#ifdef TTH_DIAGNOSTIC_BUILD
  _mockTurn.attachSynthScratch(_synthScratch, TTH_PLAYBACK_CHUNK_SAMPLES);
#endif

  Serial.printf(
      "[capture] turn buffer %lu bytes PSRAM (%u s at %u Hz), 2 x %lu-sample "
      "DMA chunks (%lu ms each)\r\n",
      static_cast<unsigned long>(TTH_TURN_BUFFER_BYTES),
      static_cast<unsigned>(TTH_CAPTURE_MAX_SECONDS + TTH_CAPTURE_MARGIN_SECONDS),
      static_cast<unsigned>(TTH_MIC_SAMPLE_RATE),
      static_cast<unsigned long>(TTH_CAPTURE_CHUNK_SAMPLES),
      static_cast<unsigned long>((TTH_CAPTURE_CHUNK_SAMPLES * 1000u) /
                                 TTH_MIC_SAMPLE_RATE));
  Serial.printf(
      "[play] %lu x %lu-sample DMA slots (%lu bytes); rate per stream, never "
      "fixed\r\n",
      static_cast<unsigned long>(PcmPlayer::kSlots),
      static_cast<unsigned long>(TTH_PLAYBACK_CHUNK_SAMPLES),
      static_cast<unsigned long>(PcmPlayer::kSlots * playbackBytes));
  return true;
}

void App::beginTurn(uint32_t nowMs) {
  // LISTENING is entered ONLY after the microphone is actually recording. A
  // face that says "I am listening" while the microphone failed to start would
  // be a lie the child cannot detect.
  TTH_TIME_BLOCK("beginTurn");
  markTransitionIteration();

  if (!startCaptureTurn(nowMs)) {
    const ConversationState previous = _machine.state();
    logStateIfChanged(_startFailureTransient
                          ? _machine.onTransientError(nowMs, TTH_TURN_ERROR_HOLD_MS)
                          : _machine.onError(),
                      previous);
    return;
  }

  {
    TTH_TIME_BLOCK("state transition");
    const ConversationState previous = _machine.state();
    logStateIfChanged(_machine.onPress(nowMs), previous);
  }

  // The face must react on the press, not at the next animation frame, so the
  // immediate render is part of the press cost and is measured as such.
  {
    TTH_TIME_BLOCK("press face render");
    serviceFace(nowMs);
  }
}

bool App::startCaptureTurn(uint32_t nowMs) {
  _startFailureTransient = false;
  // Logged BEFORE the work, so the log reads in causal order:
  //   PTT PRESS -> capture start -> mic recording -> stream -> state listening
  _log.printf("[capture] start requested: %u Hz mono s16le, capacity %lu bytes",
              static_cast<unsigned>(TTH_MIC_SAMPLE_RATE),
              static_cast<unsigned long>(_capture.capacityBytes()));

  bool started = false;
  {
    // capture.start = AudioBus::acquireMic() + IAudioCapture::startCapture(),
    // including the one-off 32 ms first-frame priming documented in Phase 4.
    TTH_TIME_BLOCK("capture.start");
    started = _capture.start(nowMs);
  }
  if (!started) {
    _log.printf("[capture] START FAILED (%s)",
                toString(_capture.stopReason()));
    return false;
  }

  _lastCaptureStatusMs = nowMs;
  _reportGate.onStart();

  // Streaming begins with the recording, not after it: the streamer follows
  // the committed length of the same buffer the capture is filling.
  if (!_streamer.begin(_capture.turn(), *_turnSource, _captureFormat)) {
    _log.printf("[stream] could not begin (%s refused the turn)", sourceName());
    // The gateway refusing (session not ready, outbound full) is transient.
    _startFailureTransient = usingGateway();
    // Hand the microphone straight back through the normal cooperative stop,
    // so the drain and the summary work exactly as for any other turn.
    _reportGate.onStopRequested();
    _capture.stop(nowMs, CaptureStopReason::AudioError);
    return false;
  }
  _log.printf("[stream] streaming to %s: %u-sample frames", sourceName(),
              static_cast<unsigned>(TTH_STREAM_FRAME_SAMPLES));
  return true;
}

void App::finishTurn(uint32_t nowMs, CaptureStopReason reason) {
  if (!_capture.isCapturing()) {
    // Already stopped itself (buffer full, read failure, its own ceiling), or
    // the press that began this hold was ignored; nothing to finish.
    return;
  }

  // Short, immediate acknowledgement. The SUMMARY is NOT printed here: the
  // microphone is still draining and the bus still reads `mic`, so a summary
  // now would describe a state that has not happened yet. serviceCapture()
  // emits it once the hardware is genuinely released.
  markTransitionIteration();
  _reportGate.onStopRequested();
  _log.printf("[capture] stop requested reason=%s", toString(reason));

  // The UI moves on immediately; only the hardware cleanup is deferred. The
  // state change is logged BEFORE capture.stop() so the log reads in causal
  // order even when the drain happens to complete in the same call:
  //   PTT RELEASE -> stop requested -> state waiting -> mic stopped -> SUMMARY
  const ConversationState previous = _machine.state();
  logStateIfChanged(_machine.onRelease(nowMs), previous);

  {
    TTH_TIME_BLOCK("capture.stop");
    _capture.stop(nowMs, reason);
  }
  // The streamer is deliberately NOT stopped: it keeps draining the committed
  // tail, and only then tells the source the user turn is over.
}

void App::serviceCapture(uint32_t nowMs) {
  const bool wasRecording = _capture.isCapturing();

  if (_capture.isBusy()) {
    const bool draining = _capture.isDraining();
    const uint32_t pollStart = micros();
    {
      TTH_TIME_BLOCK("capture.poll");
      _capture.poll(nowMs);
    }
    if (draining) {
      // One cooperative drain poll. The expensive one is the last, which is
      // where AudioBus::releaseAll() -> M5.Mic.end() actually runs; that is
      // why M5.Mic.end is timed separately.
      const uint32_t elapsed = micros() - pollStart;
      if (elapsed > _maxDrainPollMicros) _maxDrainPollMicros = elapsed;
    }
  }

  // The controller ended the turn by itself (read failure, full buffer, or its
  // own ceiling). Acknowledge it now; the SUMMARY waits for the release.
  if (wasRecording && !_capture.isCapturing()) {
    const CaptureStopReason reason = _capture.stopReason();
    _reportGate.onStopRequested();
    _log.printf("[capture] stop requested reason=%s (by controller)",
                toString(reason));

    const ConversationState previous = _machine.state();
    if (reason == CaptureStopReason::AudioError) {
      // A broken recording is not sent on as if it were complete.
      _streamer.abandon();
      _turnSource->cancel();
      logStateIfChanged(_machine.onError(), previous);
    } else {
      // A full buffer or the controller's own ceiling still produced a usable
      // turn, so it completes normally.
      logStateIfChanged(_machine.onRelease(nowMs), previous);
    }
  }

  // THE SUMMARY IS EMITTED HERE AND ONLY HERE: on the transition to fully
  // released. Before this point the bus still reads `mic`, and a summary
  // claiming otherwise would be describing a state that has not happened.
  if (_reportGate.takeSummaryDue(_capture.isBusy())) {
    printCaptureSummary();
  }

  // Routine status stops the moment a stop is requested, so a forced stop can
  // never log another 45000 ms status line after its own acknowledgement.
  if (!_reportGate.shouldPrintStatus()) return;
  if (!_capture.isCapturing()) return;
  if ((nowMs - _lastCaptureStatusMs) < TTH_CAPTURE_STATUS_MS) return;
  _lastCaptureStatusMs = nowMs;

  const CaptureMetrics& m = _capture.metrics();
  _log.printf(
      "[capture] %lums samples=%lu bytes=%lu rms=%.4f peak=%.4f failed=%lu "
      "audio=%s maxRead=%luus streamLag=%luB",
      static_cast<unsigned long>(m.durationMs),
      static_cast<unsigned long>(m.samples), static_cast<unsigned long>(m.bytes),
      static_cast<double>(m.rms), static_cast<double>(m.peak),
      static_cast<unsigned long>(m.failedReads), toString(_audioBus.owner()),
      static_cast<unsigned long>(_microphone.maxReadMicros()),
      static_cast<unsigned long>(_streamer.lagSamples() * 2u));
}

void App::serviceStream(uint32_t nowMs) {
  if (!_streamer.isActive()) return;

  // "Producer done" means the capture will never append again. Draining does
  // not append, so only Recording counts as still producing.
  const bool producerDone = !_capture.isCapturing();

  StreamStatus status = StreamStatus::Streaming;
  {
    TTH_TIME_BLOCK("stream.service");
    status = _streamer.service(producerDone);
  }

  if (status == StreamStatus::Finished) {
    printStreamSummary("DONE");
    return;
  }
  if (status == StreamStatus::Failed) {
    printStreamSummary("FAILED");
    if (_capture.isCapturing()) {
      _reportGate.onStopRequested();
      _capture.stop(nowMs, CaptureStopReason::AudioError);
    }
    failTurn(nowMs, "turn source rejected the user audio", usingGateway());
  }
}

void App::serviceTurnSource(uint32_t nowMs) {
  _turnSource->poll(nowMs);

  // M4: the gateway ran its credit down to zero (at most one line per 2 s).
  const uint32_t stalls = _gateway.stats().zeroCreditStalls;
  if (stalls != _reportedZeroStalls && (nowMs - _lastZeroStallReportMs) >= 2000u) {
    _reportedZeroStalls = stalls;
    _lastZeroStallReportMs = nowMs;
    logMemoryPoint("zero-credit stall");
  }

  // During a barge-in the old turn's events are left where they are:
  // cancelTurnSource() moves the source to a new generation, and they are then
  // discarded as stale rather than acted on.
  if (_bargeIn.isActive()) return;

  TurnEvent event;
  while (_turnSource->nextEvent(event)) handleTurnEvent(event, nowMs);

  // The speaker may start only once the microphone is fully released -- a
  // capture that is still draining owns the shared hardware.
  if (_speechStartPending && !_capture.isBusy()) startPlayback(nowMs);

  pumpPlayback(nowMs);
}

void App::handleTurnEvent(const TurnEvent& event, uint32_t nowMs) {
  switch (event.type) {
    case TurnEventType::SpeechStart:
      _log.printf("[turn] speech start: %lu Hz mono s16le (%s)",
                  static_cast<unsigned long>(event.format.sampleRate), sourceName());
      if (_machine.state() != ConversationState::Waiting) {
        _log.printf("[turn] speech start ignored in %s",
                    toString(_machine.state()));
        return;
      }
      _speechStartPending = true;
      _pendingSpeechFormat = event.format;
      if (usingGateway()) logMemoryPoint("downstream buffering");
#ifdef TTH_DIAGNOSTIC_BUILD
      if (!usingGateway() && _mockTurn.turnMode() == MockMode::Loopback) {
        // The measured input level, and the one gain chosen for this turn.
        _log.printf("[loopback] turn level: peak=%.3f p99.9=%.3f -> gain "
                    "%+.1f dB configured, %+.1f dB applied",
                    static_cast<double>(_mockTurn.turnInputPeak()) /
                        kPcmFullScale,
                    static_cast<double>(_mockTurn.turnRobustPeak()) /
                        kPcmFullScale,
                    static_cast<double>(_mockTurn.configuredTurnGainDb()),
                    static_cast<double>(_mockTurn.appliedGainDb()));
      }
#endif
      return;

    case TurnEventType::TurnComplete:
      _log.printf("[turn] complete: %s delivered %lu samples", sourceName(),
                  static_cast<unsigned long>(
#ifdef TTH_DIAGNOSTIC_BUILD
                      !usingGateway() ? _mockTurn.responseSamples() :
#endif
                      _gatewayTurn.lastResponseBytes() / 2u));
      if (_player.isStreaming()) {
        // Every chunk has been handed to the player; play out what it holds.
        _player.markEndOfStream(nowMs);
      } else if (!_speechStartPending && _player.isIdle()) {
        // A response with no speech at all.
        const ConversationState previous = _machine.state();
        logStateIfChanged(_machine.onTurnComplete(), previous);
      }
      return;

    case TurnEventType::Error:
      _log.printf("[turn] ERROR from source: %s", toString(event.error));
      failTurn(nowMs, toString(event.error), isTransient(event.error));
      return;

    case TurnEventType::None:
      return;
  }
}

void App::startPlayback(uint32_t nowMs) {
  _speechStartPending = false;
  if (_machine.state() != ConversationState::Waiting) return;

  markTransitionIteration();
  _speakerOutput.markNewStream();

  // The physical speaker level this response plays at, for its summary.
  _streamVolume = _audioDevice.masterVolume();

  bool opened = false;
  {
    // Includes AudioBus::acquireSpeaker() -> M5.Speaker.begin, timed inside.
    TTH_TIME_BLOCK("playback.open");
    opened = _player.openStream(_pendingSpeechFormat, nowMs);
  }
  if (!opened) {
    _log.printf("[play] OPEN FAILED: %lu Hz stream, bus owner %s",
                static_cast<unsigned long>(_pendingSpeechFormat.sampleRate),
                toString(_audioBus.owner()));
    failTurn(nowMs, "speaker could not be opened", false);
    return;
  }

  _acceptPlayback = true;
  _speechStartMs = nowMs;
  _log.printf("[play] stream open: %lu Hz mono s16le, %lu x %lu-sample slots, "
              "audio=%s master=%u (path %.1f dB)",
              static_cast<unsigned long>(_player.format().sampleRate),
              static_cast<unsigned long>(PcmPlayer::kSlots),
              static_cast<unsigned long>(_player.slotSamples()),
              toString(_audioBus.owner()),
              static_cast<unsigned>(_streamVolume),
              pathGainDbAt(_streamVolume));

  const ConversationState previous = _machine.state();
  logStateIfChanged(_machine.onSpeechStart(), previous);
}

void App::pumpPlayback(uint32_t nowMs) {
  if (!_acceptPlayback || !_player.isStreaming()) return;

  TTH_TIME_BLOCK("playback.pump");
  // Peek, copy, and only then consume. A full ring leaves the samples with
  // the source to be offered again, so nothing is lost to backpressure here
  // either.
  for (uint32_t i = 0; i < PcmPlayer::kSlots && _player.canAccept(); ++i) {
    AudioChunk chunk;
    if (!_turnSource->peekPlaybackChunk(chunk, _player.slotSamples())) return;

    uint32_t accepted = 0;
    const PlayerPush result = _player.submit(chunk, accepted);
    if (result == PlayerPush::Accepted) {
      _turnSource->consumePlayback(accepted);
      continue;
    }
    if (result == PlayerPush::Busy) return;

    // FormatMismatch or NotOpen: refused, never played.
    _log.printf("[play] chunk REJECTED (%s): chunk %lu Hz, stream %lu Hz",
                toString(result),
                static_cast<unsigned long>(chunk.format.sampleRate),
                static_cast<unsigned long>(_player.format().sampleRate));
    failTurn(nowMs, "playback format mismatch", false);
    return;
  }
}

void App::servicePlayback(uint32_t nowMs) {
  if (_player.isIdle()) return;

  {
    TTH_TIME_BLOCK("playback.service");
    _player.service(nowMs);
  }

  // Measured from what the SPEAKER accepted, not from the SpeechStart event.
  // (No vibration here: the robot does not buzz when it starts speaking.)
  if (_player.consumeFirstAudio()) {
    _log.printf("[play] first audio accepted by speaker %lu ms after speech "
                "start",
                static_cast<unsigned long>(nowMs - _speechStartMs));
    if (usingGateway()) logMemoryPoint("playback");
  }

  // A barge-in releases the speaker itself, between its own steps.
  if (_player.isDrained() && !_bargeIn.isActive()) finishPlayback(nowMs);
}

void App::finishPlayback(uint32_t nowMs) {
  (void)nowMs;
  markTransitionIteration();
  const PlaybackEnd reason = _player.endReason();
  {
    // Includes AudioBus::releaseAll() -> M5.Speaker.end, timed inside.
    TTH_TIME_BLOCK("playback.release");
    _player.release();
  }
  _acceptPlayback = false;
  printPlaybackSummary();

  if (reason == PlaybackEnd::Completed) {
    const ConversationState previous = _machine.state();
    logStateIfChanged(_machine.onTurnComplete(), previous);
  }
  // Error: the machine is already in Error (failTurn). Cancelled: only a
  // barge-in cancels, and it handles its own state.
}

void App::failTurn(uint32_t nowMs, const char* why, bool transient) {
  _log.printf("[turn] FAILED: %s%s", why,
              transient ? " (ERROR for the hold, then back to rest)" : "");
  markTransitionIteration();
  _acceptPlayback = false;
  _speechStartPending = false;
  _streamer.abandon();
  _turnSource->cancel();
  // A recording still running stops through the normal cooperative drain, so
  // the microphone is released and its summary printed.
  if (_capture.isCapturing()) {
    _reportGate.onStopRequested();
    _capture.stop(nowMs, CaptureStopReason::TurnFailed);
  }
  // The speaker is stopped, drained and released through the normal path in
  // servicePlayback(), which also prints its summary.
  _player.cancel(nowMs, PlaybackEnd::Error);

  const ConversationState previous = _machine.state();
  logStateIfChanged(transient ? _machine.onTransientError(nowMs, TTH_TURN_ERROR_HOLD_MS)
                              : _machine.onError(),
                    previous);
}

void App::serviceBargeIn(uint32_t nowMs) {
  if (!_bargeIn.isActive()) return;

  const BargeInResult result = _bargeIn.poll(nowMs);
  if (result == BargeInResult::Pending) return;

  markTransitionIteration();
  _log.printf("[barge] %s: speaker quiet after %lu ms, press->mic %lu us",
              toString(result), static_cast<unsigned long>(_bargeIn.lastDrainMs()),
              static_cast<unsigned long>(_bargeIn.lastTotalMicros()));

  const ConversationState previous = _machine.state();
  switch (result) {
    case BargeInResult::Listening:
      // Only now: speaker stopped and released, turn source cancelled,
      // microphone recording.
      logStateIfChanged(_machine.onPress(nowMs), previous);
      {
        TTH_TIME_BLOCK("press face render");
        serviceFace(nowMs);
      }
      return;
    case BargeInResult::ReleasedEarly:
      logStateIfChanged(_machine.onTurnComplete(), previous);
      return;
    case BargeInResult::Failed:
      logStateIfChanged(_startFailureTransient
                            ? _machine.onTransientError(nowMs, TTH_TURN_ERROR_HOLD_MS)
                            : _machine.onError(),
                        previous);
      return;
    case BargeInResult::Idle:
    case BargeInResult::Pending:
      return;
  }
}

// --- IBargeInOps -------------------------------------------------------------

void App::stopAcceptingPlayback() {
  _acceptPlayback = false;
  _speechStartPending = false;
}

void App::stopSpeaker(uint32_t nowMs) {
  _player.cancel(nowMs, PlaybackEnd::Cancelled);
}

bool App::speakerQuiet() {
  // servicePlayback() runs every loop and moves the player from Stopping to
  // Drained once the speaker holds nothing.
  return _player.isDrained() || _player.isIdle();
}

void App::cancelTurnSource() {
  // Gateway: the ring is purged of the turn, its credit returned, and the
  // cancel queued through the ordered path. No acknowledgement is awaited.
  _streamer.abandon();
  _turnSource->cancel();
}

void App::releaseSpeaker() {
  if (!_player.isDrained()) return;
  {
    TTH_TIME_BLOCK("playback.release");
    _player.release();
  }
  printPlaybackSummary();
}

bool App::startCapture(uint32_t nowMs) { return startCaptureTurn(nowMs); }

bool App::stillHeld() const { return _ptt.isHeld(); }

// --- reports -----------------------------------------------------------------

void App::printCaptureSummary() {
  // TWO lines, explicitly named, queued as CRITICAL.
  //
  // As one line this reached ~218 characters at worst against a 224-byte slot,
  // and the fields at the end -- dropped, highWater, audio, drainTotal -- are
  // exactly the ones that prove the turn completed and the microphone was
  // released. Truncating those would leave a report that still looks
  // authoritative while having lost its conclusion.
  //
  // pushCritical() may use the reserved slots, so routine chatter can never
  // crowd the report out; tth/CaptureSummary.h holds the formatting, and the
  // tests assert both lines fit at absurd worst case.
  const CaptureMetrics& m = _capture.metrics();

  CaptureSummaryData data;
  data.reason = toString(_capture.stopReason());
  data.durationMs = m.durationMs;
  data.samples = m.samples;
  data.bytes = m.bytes;
  data.highWaterBytes = _capture.highWaterBytes();
  data.minRms = m.minRms;
  data.averageRms = m.averageRms;
  data.maxRms = m.maxRms;
  data.peak = m.absolutePeak;
  data.chunks = m.chunks;
  data.failedReads = m.failedReads;
  data.droppedSamples = m.droppedSamples;
  // Read at emission time, which is after the release -- so this genuinely
  // reports the bus state rather than predicting it.
  data.audioOwner = toString(_audioBus.owner());
  data.drainTotalMs = _capture.lastDrainTotalMs();

  char line[kLogMessageMax];
  formatCaptureSummaryLine1(line, sizeof(line), data);
  _log.queue().pushCritical(line);
  formatCaptureSummaryLine2(line, sizeof(line), data);
  _log.queue().pushCritical(line);
}

void App::printStreamSummary(const char* outcome) {
  // A separate line from the capture SUMMARY, on purpose: the stream may
  // legitimately outlive the capture (that is the TurnBuffer lifetime rule),
  // so it reports when IT is done, not when the microphone was released.
  const StreamMetrics& s = _streamer.metrics();
  char line[kLogMessageMax];
  snprintf(line, sizeof(line),
           "[stream] %s samples=%lu bytes=%lu frames=%lu busyRetries=%lu "
           "maxLag=%luB committed=%lu | %s queued frames=%lu bytes=%lu",
           outcome, static_cast<unsigned long>(s.streamedSamples),
           static_cast<unsigned long>(s.streamedSamples * 2u),
           static_cast<unsigned long>(s.frames),
           static_cast<unsigned long>(s.busyRetries),
           static_cast<unsigned long>(s.maxLagSamples * 2u),
           static_cast<unsigned long>(_capture.turn().committedSamples()),
           sourceName(), static_cast<unsigned long>(_gatewayTurn.upFrames()),
           static_cast<unsigned long>(_gatewayTurn.upBytes()));
  _log.queue().pushCritical(line);
#ifdef TTH_DIAGNOSTIC_BUILD
  if (!usingGateway()) {
    snprintf(line, sizeof(line), "[stream] mock received=%lu gaps=%lu",
             static_cast<unsigned long>(_mockTurn.receivedSamples()),
             static_cast<unsigned long>(_mockTurn.continuityErrors()));
    _log.queue().pushCritical(line);
  }
#endif
}

void App::printPlaybackSummary() {
  // TWO named lines, queued as CRITICAL, emitted after release() so
  // `audio=` reports the real bus state. tth/PlaybackSummary.h holds the
  // formatting, and a test proves neither line can be truncated.
  const PlaybackMetrics& p = _player.metrics();

  PlaybackSummaryData data;
  data.end = toString(_player.endReason());
  data.rate = _player.format().sampleRate;
  data.queued = p.queued;
  data.played = p.played;
  data.samples = p.samplesQueued;
  data.underruns = p.underruns;
  data.rejects = p.formatRejects;
  data.refusals = p.playRefusals;
  data.audioOwner = toString(_audioBus.owner());
  data.drainMs = _player.lastDrainMs();
  // Loudness: inputPeak is what the source had before any gain;
  // outputPeak is what PcmPlayer actually received.
  // Gateway audio is played as received: no source-side gain.
  data.source = sourceName();
  data.gainDb = 0.0f;
  data.inputPeak = static_cast<float>(p.inputPeak) / kPcmFullScale;
  data.outputPeak = static_cast<float>(p.inputPeak) / kPcmFullScale;
  data.limitedSamples = 0;
#ifdef TTH_DIAGNOSTIC_BUILD
  if (!usingGateway()) {
    data.source = toString(_mockTurn.turnMode());
    data.gainDb = _mockTurn.appliedGainDb();
    data.inputPeak =
        static_cast<float>(_mockTurn.responseInputPeak()) / kPcmFullScale;
    data.limitedSamples = _mockTurn.limitedSamples();
  }
#endif
  data.maxLevel = p.maxLevel;
  // The physical speaker level this response played at.
  data.masterVolume = _streamVolume;
  data.pathGainDb = static_cast<float>(pathGainDbAt(_streamVolume));

  char line[kLogMessageMax];
  formatPlaybackSummaryLine1(line, sizeof(line), data);
  _log.queue().pushCritical(line);
  formatPlaybackSummaryLine2(line, sizeof(line), data);
  _log.queue().pushCritical(line);
}

void App::serviceSerialCommands(uint32_t nowMs) {
  _serialLines.poll(nowMs);
  // Bounded per loop: however much is pasted, serial input can never stall
  // the cooperative loop. The rest waits in the UART buffer.
  for (uint32_t i = 0; i < TTH_SERIAL_BYTES_PER_LOOP && Serial.available() > 0;
       ++i) {
    const int raw = Serial.read();
    if (raw < 0) break;
    const SerialEvent event =
        _serialLines.feed(static_cast<uint8_t>(raw), nowMs);
    switch (event.type) {
      case SerialEventType::Key:
        handleDiagnosticKey(event.key, nowMs);
        break;
      case SerialEventType::Line:
        handleProvisioningLine(event.line, event.length, nowMs);
        // The line may have carried a credential: zero it now.
        _serialLines.wipe();
        break;
      case SerialEventType::Overflow:
        _log.queue().pushCritical(
            "[prov] ERROR: line too long; discarded (nothing changed)");
        break;
      case SerialEventType::None:
        break;
    }
  }
}

void App::handleDiagnosticKey(char key, uint32_t nowMs) {

  // Everything here goes through the shared log queue, so key responses stay
  // in order with the events around them.
  if (_faceOverride.handleKey(key, nowMs)) {
    if (_faceOverride.isCycling()) {
      _log.printf("[face] override: automatic cycle");
    } else {
      _log.printf("[face] override: %s",
                  toString(_faceOverride.faceFor(_shownFace, nowMs)));
    }
    _log.printf("[face] (press the push-to-talk input to return to the real "
                "state machine)");
    return;
  }

  if (key == 'b' || key == 'B') {
    _fixedLevelIndex = nextFixedLevelIndex(_fixedLevelIndex);
    // Stepping the level implies wanting to look at the bars, so force the
    // speaking face too.
    _faceOverride.handleKey('s', nowMs);
    _speechSmoother.reset(0.0f);
    const float level = fixedSpeechLevelFor(_fixedLevelIndex);
    if (level < 0.0f) {
      _log.printf("[face] bar level: simulated envelope");
    } else {
      _log.printf("[face] bar level: %.2f (fixed)", static_cast<double>(level));
    }
    return;
  }

#ifdef TTH_DIAGNOSTIC_BUILD
  // DIAGNOSTIC BUILD ONLY. A production image has none of these keys, so
  // nothing typed on the serial port can switch it away from the gateway.
  if (key == 't' || key == 'T') {
    if (!provisioningWritesAllowed()) {
      _log.printf("[turn] source not changed: a turn is in progress");
      return;
    }
    _useMock = !_useMock;
    _turnSource = _useMock ? static_cast<ITurnSource*>(&_mockTurn) : &_gatewayTurn;
    _log.printf("[turn] DIAGNOSTIC source: %s", sourceName());
    return;
  }

  if (key == 'm' || key == 'M') {
    _mockTurn.setMode(_mockTurn.mode() == MockMode::Loopback
                          ? MockMode::Synthetic
                          : MockMode::Loopback);
    _log.printf("[turn] mock mode: %s (from the next user turn)",
                toString(_mockTurn.mode()));
    return;
  }

  if (key == 'p' || key == 'P') {
    _mockTurn.setBackpressure(!_mockTurn.backpressure());
    _log.printf("[turn] simulated backpressure: %s (Busy %u ms of every %u ms)",
                _mockTurn.backpressure() ? "ON" : "off",
                static_cast<unsigned>(TTH_MOCK_BUSY_WINDOW_MS),
                static_cast<unsigned>(TTH_MOCK_BUSY_PERIOD_MS));
    return;
  }

  if (key == 'x' || key == 'X') {
    _mockTurn.injectErrorOnNextTurn();
    _log.printf("[turn] the next mock response will be an injected ERROR");
    return;
  }

  if (key == 'g' || key == 'G') {
    // A/B steps for choosing the final loopback gain.
    static const float kSteps[4] = {0.0f, 6.0f, 9.0f, 12.0f};
    const float current = _mockTurn.loopbackGainDb();
    uint32_t next = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (current > kSteps[i] - 0.5f && current < kSteps[i] + 0.5f) {
        next = (i + 1) % 4;
      }
    }
    _mockTurn.setLoopbackGainDb(kSteps[next]);
    _log.printf("[loopback] gain: %+.1f dB (from the next user turn)",
                static_cast<double>(_mockTurn.loopbackGainDb()));
    return;
  }
#endif

  if (key == '?' || key == 'h' || key == 'H') {
    printFaceCommandHelp();
    return;
  }
  // Newlines and stray bytes are ignored.
}

void App::serviceFace(uint32_t nowMs) {
  if (_activitySelector.isOpen()) {
    // The menu owns the display; the face is restored when it closes.
    if (_activityMenuDirty) {
      _activityMenuDirty = false;
      TTH_TIME_BLOCK("activity menu render");
      drawActivityMenu();
    }
    return;
  }
  const FaceState productionFace = faceStateFor(_machine.state());
  const FaceState face = _faceOverride.faceFor(productionFace, nowMs);

  // A state change is handled on the spot rather than waiting for the next
  // animation frame, so the face reacts the instant the button goes down.
  if (face != _shownFace) {
    _shownFace = face;
    _faceEnteredMs = nowMs;
    _faceAnimator.setState(face, nowMs);
    // Entering Speaking starts from silent bars; leaving it must not leave a
    // stale level behind for the next turn.
    _speechSmoother.reset(0.0f);
    _faceTransitionPending = true;
  } else if ((nowMs - _lastFaceFrameMs) < TTH_FACE_FRAME_MS) {
    // Continuous animation is rate-limited; transitions are not.
    return;
  }

  const uint32_t dtMs = nowMs - _lastFaceFrameMs;
  _lastFaceFrameMs = nowMs;

  // The cheek bars. In production they follow the REAL playback amplitude --
  // the RMS of the chunk the speaker is playing now. The simulated envelope
  // and the fixed levels remain, but only for the 's' / 'b' diagnostics. The
  // smile itself never changes; only this one number does.
  float speechLevel = 0.0f;
  if (face == FaceState::Speaking) {
    float raw = 0.0f;
    if (_faceOverride.isActive()) {
      const float fixed = fixedSpeechLevelFor(_fixedLevelIndex);
      raw = (fixed >= 0.0f) ? fixed
                            : simulatedSpeechAmplitude(nowMs - _faceEnteredMs);
    } else {
      raw = _player.currentLevel() * TTH_BAR_LEVEL_GAIN;
      if (raw > 1.0f) raw = 1.0f;
    }
    speechLevel = _speechSmoother.update(raw, dtMs);
  }

  const bool atomic = _faceTransitionPending;
  _faceTransitionPending = false;
  _faceRenderer.render(_faceAnimator.update(nowMs, speechLevel), atomic);
}

void App::logStateIfChanged(bool changed, ConversationState previous) {
  if (!changed) return;
  // Through the shared queue like everything else. Writing this directly to
  // Serial was half of why the log read out of order: a direct write appeared
  // at once while the queued "[ptt] PRESS" that caused it was still waiting.
  _log.printf("[app] state %s -> %s (face: %s)", toString(previous),
              toString(_machine.state()),
              toString(faceStateFor(_machine.state())));

  // M3/M4 memory points of a gateway turn.
  if (!usingGateway()) return;
  switch (_machine.state()) {
    case ConversationState::Listening:
      logMemoryPoint("capture/upstream");
      return;
    case ConversationState::Waiting:
      logMemoryPoint("waiting");
      return;
    case ConversationState::Ready:
      if (previous == ConversationState::Speaking || previous == ConversationState::Waiting ||
          previous == ConversationState::Error) {
        logMemoryPoint("back to ready");
      }
      return;
    default:
      return;
  }
}

void App::serviceNetwork(uint32_t nowMs) {
  {
    TTH_TIME_BLOCK("wifi.poll");
    _wifi.poll(nowMs);
  }
  // Driver calls are rare, deliberate transitions; they are never counted as
  // steady-state loop time.
  if (_wifi.consumeTransition()) markTransitionIteration();

  // --- the gateway session (Step 6.2) -----------------------------------------
  // The network task does the blocking work; here the loop only moves events
  // and decisions through queues.
  {
    const SessionState before = _session.state();
    _session.setNetworkUp(_wifi.connectivity() == Connectivity::Online, nowMs);
    logSessionChange(before, nowMs);
  }
  {
    TTH_TIME_BLOCK("gateway.events");
    for (uint32_t i = 0;
         i < TTH_GW_EVENTS_PER_LOOP && _gateway.nextEvent(_gatewayEvent); ++i) {
      handleGatewayEvent(nowMs);
    }
  }
  const bool conversationIdle = provisioningWritesAllowed();
  for (int i = 0; i < 4; ++i) {
    const SessionState before = _session.state();
    const SessionAction action = _session.poll(nowMs, conversationIdle);
    if (action == SessionAction::None) break;
    executeSessionAction(action, nowMs);
    logSessionChange(before, nowMs);
  }
  syncTurnSourceWithSession(nowMs);
  updateConnectivity();
}

bool App::allocateTrustAnchorBuffers() {
  // PSRAM: ~16 KB that TLS would otherwise compete with in internal RAM. Only
  // NVS reads/writes (which bounce through internal buffers) and our own code
  // touch them.
  _caActive = static_cast<char*>(
      heap_caps_calloc(1, trust::kMaxPemBytes + 1, MALLOC_CAP_SPIRAM));
  _caScratchA = static_cast<uint8_t*>(
      heap_caps_calloc(1, TrustAnchorStore::kScratchBytes, MALLOC_CAP_SPIRAM));
  _caScratchB = static_cast<uint8_t*>(
      heap_caps_calloc(1, TrustAnchorStore::kScratchBytes, MALLOC_CAP_SPIRAM));
  _caStaging = static_cast<char*>(
      heap_caps_calloc(1, trust::kMaxPemBytes + 1, MALLOC_CAP_SPIRAM));
  if (_caActive == nullptr || _caScratchA == nullptr || _caScratchB == nullptr ||
      _caStaging == nullptr) {
    return false;
  }
  _trustStore.attachBuffers(_caActive, _caScratchA, _caScratchB);
  return true;
}

void App::applyGatewayTarget(uint32_t nowMs) {
  const bool haveConfig = _configStore.hasConfig();
  const bool haveAnchor = _trustStore.hasAnchor();
  if (haveConfig && haveAnchor) {
    const config::DeviceConfig& c = _configStore.active();
    _gateway.setTarget(c, _trustStore.pem(), _trustStore.length());
    config::GatewayUrlParts parts;
    config::checkGatewayUrl(c.gatewayUrl, c.gatewayUrlLength, &parts);
    _log.printf("[gw] target wss://%.64s:%u%.48s as device %s (CA: %u certificate(s))",
                parts.host, static_cast<unsigned>(parts.port), parts.path, c.deviceId,
                static_cast<unsigned>(_trustStore.certificates()));
  } else {
    _gateway.clearTarget();
    if (!haveConfig) {
      _log.printf("[gw] disabled: not provisioned");
    } else {
      _log.printf("[gw] disabled: no CA provisioned (tools/provision.py --ca-only "
                  "<ca.pem>)");
    }
  }
  const SessionState before = _session.state();
  _session.configure(haveConfig && haveAnchor, nowMs);
  logSessionChange(before, nowMs);
}

void App::handleGatewayEvent(uint32_t nowMs) {
  const GatewayClient::Event& e = _gatewayEvent;
  if (e.generation != _gatewayGeneration) {
    // From a connection the session has already moved past.
    ++_staleGatewayEvents;
    return;
  }
  const SessionState before = _session.state();
  switch (e.type) {
    case GatewayClient::EventType::Connected:
      _log.printf("[gw] TLS + WebSocket upgrade ok in %lu ms (CA and host name "
                  "verified; internal free=%lu largest=%lu low-water %lu->%lu)",
                  static_cast<unsigned long>(e.elapsedMs),
                  static_cast<unsigned long>(e.heapFree),
                  static_cast<unsigned long>(e.heapLargest),
                  static_cast<unsigned long>(e.lowWaterBefore),
                  static_cast<unsigned long>(e.lowWaterAfter));
      _session.onConnected(nowMs);
      break;

    case GatewayClient::EventType::ConnectFailed:
      if (isHttpFailure(e.failure)) {
        _log.printf("[gw] connect failed after %lu ms: http %ld (%s)",
                    static_cast<unsigned long>(e.elapsedMs), static_cast<long>(e.detail),
                    toString(e.failure));
      } else if (e.failure == ConnectFailure::Tls ||
                 e.failure == ConnectFailure::TlsAlloc) {
        _log.printf("[gw] connect failed after %lu ms: %s (mbedtls %ld, internal "
                    "free=%lu largest=%lu low-water %lu->%lu)",
                    static_cast<unsigned long>(e.elapsedMs), toString(e.failure),
                    static_cast<long>(e.detail), static_cast<unsigned long>(e.heapFree),
                    static_cast<unsigned long>(e.heapLargest),
                    static_cast<unsigned long>(e.lowWaterBefore),
                    static_cast<unsigned long>(e.lowWaterAfter));
      } else {
        _log.printf("[gw] connect failed after %lu ms: %s (detail %ld)",
                    static_cast<unsigned long>(e.elapsedMs), toString(e.failure),
                    static_cast<long>(e.detail));
      }
      _session.onConnectFailed(e.failure, nowMs);
      break;

    case GatewayClient::EventType::Text: {
      const wire::ControlError error =
          wire::parseControl(e.text, e.length, _controlMessage);
      if (error != wire::ControlError::None) {
        _log.printf("[gw] malformed control message (%s) -> reconnect", toString(error));
        _session.onControlError(nowMs);
        break;
      }
      switch (_controlMessage.type) {
        case wire::ControlType::Error:
          _log.printf("[gw] gateway error code=%.31s retry=%s%s", _controlMessage.code,
                      _controlMessage.retry ? "true" : "false",
                      _controlMessage.hasTurn ? " (turn-scoped)" : "");
          if (_controlMessage.hasTurn) handleTurnControl(nowMs);
          break;
        case wire::ControlType::SessionEnd:
          _log.printf("[gw] session_end reason=%.15s", _controlMessage.reason);
          break;
        case wire::ControlType::Unknown:
          _log.printf("[gw] unknown control message ignored (counted)");
          break;
        case wire::ControlType::SpeechStart:
        case wire::ControlType::TurnComplete:
        case wire::ControlType::Interrupted:
          handleTurnControl(nowMs);
          break;
        case wire::ControlType::Ready:
          // The activity the gateway is actually using for this session.
          _activityCatalog.setCurrent(_controlMessage.activity);
          break;
        case wire::ControlType::Pong:
          break;
        case wire::ControlType::ActivityList:
          handleActivityList();
          break;
        case wire::ControlType::ActivitySelected:
          handleActivitySelected(nowMs);
          break;
        case wire::ControlType::ActivitySelectError:
          handleActivitySelectError(nowMs);
          break;
      }
      _session.onControl(_controlMessage, nowMs);
      break;
    }

    case GatewayClient::EventType::Closed:
      _log.printf("[gw] connection closed: %s, code %ld, after %lu s",
                  closeCauseName(e.cause), static_cast<long>(e.detail),
                  static_cast<unsigned long>(e.elapsedMs / 1000u));
      _session.onClosed(nowMs);
      break;

    case GatewayClient::EventType::ProtocolError:
      // Detail -9: a binary frame that is not valid model audio; other
      // negative details: a text frame over 512 B.
      _log.printf("[gw] websocket protocol error (%s) -> closing",
                  e.detail == -9  ? "invalid model audio frame"
                  : e.detail < 0 ? "text frame over 512 B"
                                 : ws::toString(static_cast<ws::DecodeError>(e.detail)));
      break;

    case GatewayClient::EventType::DownstreamViolation: {
      char line[kLogMessageMax];
      snprintf(line, sizeof(line),
               "[gw] *** downstream %s violation on turn %lu: frame refused, nothing "
               "buffered was dropped; turn fails, reconnecting ***",
               e.detail == GatewayClient::kViolationCapacity ? "capacity" : "credit",
               static_cast<unsigned long>(e.turn));
      _log.queue().pushCritical(line);
      _gatewayTurn.onDownstreamViolation(nowMs);
      break;
    }

    case GatewayClient::EventType::None:
      break;
  }
  logSessionChange(before, nowMs);
}

void App::executeSessionAction(SessionAction action, uint32_t nowMs) {
  (void)nowMs;
  char text[wire::kMaxControlBytes + 1];
  switch (action) {
    case SessionAction::None:
      return;

    case SessionAction::Connect: {
      ++_gatewayGeneration;
      // BEFORE the command, while nothing is being written: the ring is
      // emptied and the credit epoch restarted for this connection.
      _gatewayTurn.onConnecting(_gatewayGeneration, nowMs);
      // A new connection sends a fresh list; a menu from the old one closes.
      _activityCatalog.reset();
      if (_activitySelector.isOpen()) handleSelectorEvent(_activitySelector.cancel(), nowMs);
      config::GatewayUrlParts parts;
      memset(&parts, 0, sizeof(parts));
      if (_configStore.hasConfig()) {
        config::checkGatewayUrl(_configStore.active().gatewayUrl,
                                _configStore.active().gatewayUrlLength, &parts);
      }
      _log.printf("[gw] connecting to wss://%.64s:%u%.48s (attempt %lu)", parts.host,
                  static_cast<unsigned>(parts.port), parts.path,
                  static_cast<unsigned long>(_session.failures() + 1u));
      if (!_gateway.connect(_gatewayGeneration)) {
        _log.printf("[gw] *** connect not queued (network task not running) ***");
      }
      return;
    }

    case SessionAction::SendHello: {
      const size_t n = wire::encodeHello(text, sizeof(text), TTH_FIRMWARE_VERSION,
                                         TTH_GW_INITIAL_CREDIT, _savedActivity);
      if (n == 0 || !_gateway.sendText(text, n)) {
        _log.printf("[gw] *** hello could not be queued ***");
      } else {
        _log.printf("[gw] hello sent (proto 1, fw %s, credit %lu B, activity %s)",
                    TTH_FIRMWARE_VERSION, static_cast<unsigned long>(TTH_GW_INITIAL_CREDIT),
                    _savedActivity[0] != '\0' ? _savedActivity : "gateway default");
      }
      return;
    }

    case SessionAction::SendPing: {
      const size_t n = wire::encodePing(text, sizeof(text), _session.pingTs());
      if (n == 0 || !_gateway.sendText(text, n)) {
        _log.printf("[gw] ping could not be queued (outbound full)");
      }
      return;
    }

    case SessionAction::Close:
      _log.printf("[gw] closing (%s)", toString(_session.lastEnd()));
      if (!_gateway.close(_gatewayGeneration)) {
        _log.printf("[gw] *** close not queued (network task not running) ***");
      }
      return;
  }
}

void App::logSessionChange(SessionState previous, uint32_t nowMs) {
  const SessionState current = _session.state();
  if (current == previous) return;
  const char* reason = _session.lastEnd() == SessionEnd::ConnectFailed
                           ? toString(_session.lastFailure())
                           : toString(_session.lastEnd());
  switch (current) {
    case SessionState::Backoff:
      _log.printf("[gw] %s -> retry in %lu ms (failures %lu)", reason,
                  static_cast<unsigned long>(_session.lastDelayMs()),
                  static_cast<unsigned long>(_session.failures()));
      return;

    case SessionState::AuthRejected:
      _log.queue().pushCritical(
          "[gw] *** AUTH REJECTED: the gateway refused this device id / token - "
          "check its registry entry; ERROR face; retrying every 300 s ***");
      return;

    case SessionState::ProtocolMismatch: {
      char line[kLogMessageMax];
      snprintf(line, sizeof(line),
               "[gw] *** PROTOCOL MISMATCH (%s): check the gateway URL and firmware; "
               "ERROR face; no retry until reprovisioned or rebooted ***",
               reason);
      _log.queue().pushCritical(line);
      return;
    }

    case SessionState::Ready: {
      _log.printf("[gw] READY session=%.39s activity=%.39s %lu ms after connect "
                  "start (sessions %lu)",
                  _session.sessionId(), _session.activityId(),
                  static_cast<unsigned long>(_session.lastReadyLatencyMs()),
                  static_cast<unsigned long>(_session.readyCount()));
      // M2 (PHASE6_PLAN §7): the first TLS session up. Every later one is
      // printed too, so stability over >= 20 reconnects can be read off.
      const diag::MemoryReport m = diag::readMemoryReport();
      _log.printf("[mem] %s gateway ready #%lu: internal free=%lu largest=%lu "
                  "min=%lu | psram free=%lu largest=%lu | tlsAllocFail=%lu",
                  _m2Reported ? "--" : "M2",
                  static_cast<unsigned long>(_session.readyCount()),
                  static_cast<unsigned long>(m.internalFree),
                  static_cast<unsigned long>(m.internalLargestBlock),
                  static_cast<unsigned long>(m.internalMinFree),
                  static_cast<unsigned long>(m.psramFree),
                  static_cast<unsigned long>(m.psramLargestBlock),
                  static_cast<unsigned long>(_gateway.stats().tlsAllocFailures));
      _m2Reported = true;
      return;
    }

    case SessionState::Idle:
      _log.printf("[gw] idle (%s)", _session.lastEnd() == SessionEnd::None
                                        ? "waiting for Wi-Fi and a gateway target"
                                        : reason);
      return;

    case SessionState::Connecting:
    case SessionState::AwaitingReady:
    case SessionState::Closing:
      // Logged where the action is executed.
      (void)nowMs;
      return;
  }
}

Connectivity App::computeConnectivity() const {
  const Connectivity gateway = _session.connectivity();
  // AuthRejected / ProtocolMismatch stay visible whatever Wi-Fi does (D4).
  if (gateway == Connectivity::Failed) return Connectivity::Failed;
  const Connectivity wifi = _wifi.connectivity();
  if (wifi != Connectivity::Online) return wifi;
  if (!_configStore.hasConfig() || !_trustStore.hasAnchor()) return Connectivity::Offline;
  return gateway;
}

void App::updateConnectivity() {
  const Connectivity next = computeConnectivity();
  if (next != _connectivity) {
    _log.printf("[net] connectivity %s -> %s", toString(_connectivity), toString(next));
    _connectivity = next;
  }
  const ConversationState previous = _machine.state();
  logStateIfChanged(_machine.setConnectivity(_connectivity), previous);
}

void App::checkLowWater(const char* stage, uint32_t nowMs) {
  const uint32_t started = micros();
  const uint32_t low = static_cast<uint32_t>(
      heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (_lowWater.observe(low, stage)) {
    // A new historical minimum. The current free figure beside it tells a
    // transient (free recovered) from a leak (free stays down).
    char line[kLogMessageMax];
    snprintf(line, sizeof(line),
             "[mem] new internal low-water %lu B (-%lu) now free=%lu largest=%lu "
             "| state=%s audio=%s gw=%s wifi=%s | first seen after: %s",
             static_cast<unsigned long>(low),
             static_cast<unsigned long>(_lowWater.lastDrop()),
             static_cast<unsigned long>(
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned long>(heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             toString(_machine.state()), toString(_audioBus.owner()),
             toString(_session.state()), toString(_wifi.state()), stage);
    _log.queue().pushCritical(line);
  }
  // The checkpoints' own cost appears in the [blocks] breakdown.
  diag::BlockStats::record("mem.checkpoint", micros() - started, nowMs);
}

void App::queueGatewayLine(uint32_t nowMs) {
  const GatewayClient::Stats g = _gateway.stats();
  char line[kLogMessageMax];
  snprintf(line, sizeof(line),
           "[gw] state=%s failures=%lu retry=%lums age=%lus sessions=%lu "
           "rtt=%lu/%lums pings=%lu stray=%lu unk=%lu proto=%lu stale=%lu | "
           "tlsAllocFail=%lu maxHs=%lums stack=%lu evDrop=%lu txDrop=%lu",
           toString(_session.state()), static_cast<unsigned long>(_session.failures()),
           static_cast<unsigned long>(_session.lastDelayMs()),
           static_cast<unsigned long>(_session.sessionAgeMs(nowMs) / 1000u),
           static_cast<unsigned long>(_session.readyCount()),
           static_cast<unsigned long>(_session.lastRttMs()),
           static_cast<unsigned long>(_session.maxRttMs()),
           static_cast<unsigned long>(_session.pingsSent()),
           static_cast<unsigned long>(_session.strayPongs()),
           static_cast<unsigned long>(_session.unknownMessages() +
                                      _session.unexpectedMessages()),
           static_cast<unsigned long>(_session.protocolErrors()),
           static_cast<unsigned long>(_staleGatewayEvents),
           static_cast<unsigned long>(g.tlsAllocFailures),
           static_cast<unsigned long>(g.maxHandshakeMs),
           static_cast<unsigned long>(g.stackHighWater),
           static_cast<unsigned long>(g.eventDrops),
           static_cast<unsigned long>(_gateway.outboundDrops()));
  _log.queue().push(line);
}

bool App::provisioningWritesAllowed() const {
  // Writing flash is a deliberate, rare action. It never lands in the middle
  // of a turn or a response.
  const ConversationState state = _machine.state();
  const bool idle = ConversationStateMachine::isIdle(state) ||
                    state == ConversationState::Error;
  return idle && !_capture.isBusy() && _player.isIdle() &&
         !_streamer.isActive() && !_bargeIn.isActive();
}

void App::handleProvisioningLine(const char* line, size_t length,
                                 uint32_t nowMs) {
  // Provisioning may write flash: never counted as steady-state loop time.
  markTransitionIteration();
  QueueLineSink sink(_log.queue());
  ProvisioningResult result;
  result.configChanged = false;
  result.showRequested = false;
  {
    TTH_TIME_BLOCK("provisioning");
    result = _provisioning.handleLine(line, length, provisioningWritesAllowed(),
                                      nowMs, sink);
  }
  if (result.showRequested) {
    queueNetLine();
    queueGatewayLine(nowMs);
  }
  if (!result.configChanged) return;

  _wifi.apply(_configStore.hasConfig() ? &_configStore.active() : nullptr,
              nowMs);
  // A new URL, identity or CA: the gateway session starts again from scratch.
  applyGatewayTarget(nowMs);
  updateConnectivity();
}

void App::queueNetLine() {
  char line[kLogMessageMax];
  snprintf(line, sizeof(line),
           "[net] wifi=%s failures=%lu lastRetryDelay=%lums rssi=%d "
           "connects=%lu | config=%s generation=%lu slot=%c",
           toString(_wifi.state()), static_cast<unsigned long>(_wifi.attempt()),
           static_cast<unsigned long>(_wifi.lastDelayMs()), _wifi.rssi(),
           static_cast<unsigned long>(_wifi.connects()),
           _configStore.hasConfig() ? "stored" : "none",
           static_cast<unsigned long>(_configStore.generation()),
           _configStore.activeSlot() != 0 ? _configStore.activeSlot() : '-');
  _log.queue().push(line);
}

void App::printFaceCommandHelp() {
  _log.printf("[keys] face: r=ready l=listening w=waiting s=speaking e=error "
              "o=sleeping a=cycle b=bar level | ?=help");
#ifdef TTH_DIAGNOSTIC_BUILD
  _log.printf("[keys] DIAGNOSTIC: t=gateway/local mock | mock: m=mode "
              "p=backpressure x=inject error g=loopback gain");
#endif
  _log.printf("[keys] provisioning: a line starting with '!' - see !prov help");
}

void App::serviceHeartbeat(uint32_t nowMs) {
  // Emitted across separate loop iterations, one line each. All at once was
  // ~250 characters of formatting and queueing in a single iteration;
  // spreading them costs nothing and keeps every iteration cheap.
  if (_heartbeatStage == 0) {
    if ((nowMs - _lastHeartbeatMs) < TTH_HEARTBEAT_MS) return;
    _lastHeartbeatMs = nowMs;

    diag::queueHeartbeat(_log.queue(), nowMs - _bootMs, _maxLoopSteadyMicros,
                         _maxLoopTransitionMicros, _maxSerialWriteMicros,
                         _loopCount,
                         toString(_machine.state()),
                         toString(_audioBus.owner()), toString(_shownFace),
                         _log.drops(), _log.queue().criticalDrops());
    _heartbeatStage = 1;
    return;
  }

  if (_heartbeatStage == 1) {
    diag::queueCaptureLine(
        _log.queue(), _faceRenderer.maxPushMicros(),
        _faceRenderer.maxTransitionMicros(), _capture.byteCount(),
        _capture.sessionHighWaterBytes(), _capture.capacityBytes(),
        _microphone.maxReadMicros(), _capture.lastDrainTotalMs(),
        _capture.maxDrainTotalMs(), _maxDrainPollMicros);
    _heartbeatStage = 2;
    return;
  }

  if (_heartbeatStage == 2) {
    const PlaybackMetrics& p = _player.metrics();
    uint32_t eventDrops = _gatewayTurn.eventDrops();
    uint32_t staleDiscards = _gatewayTurn.staleDiscards();
#ifdef TTH_DIAGNOSTIC_BUILD
    if (!usingGateway()) {
      eventDrops = _mockTurn.eventDrops();
      staleDiscards = _mockTurn.staleDiscards();
    }
#endif
    char line[kLogMessageMax];
    snprintf(line, sizeof(line),
             "[pb] player=%s queued=%lu played=%lu underruns=%lu rejects=%lu "
             "guard=%lu/%lu maxLoopSpeaking=%luus hapticsRefused=%lu | source=%s "
             "evDrop=%lu stale=%lu | barges=%lu",
             toString(_player.state()), static_cast<unsigned long>(p.queued),
             static_cast<unsigned long>(p.played),
             static_cast<unsigned long>(p.underruns),
             static_cast<unsigned long>(p.formatRejects),
             static_cast<unsigned long>(_speakerOutput.busyRefusals()),
             static_cast<unsigned long>(_speakerOutput.notRunningRefusals()),
             static_cast<unsigned long>(_maxLoopSpeakingMicros),
             static_cast<unsigned long>(_haptics.deniedCount()),
             sourceName(), static_cast<unsigned long>(eventDrops),
             static_cast<unsigned long>(staleDiscards),
             static_cast<unsigned long>(_bargeIn.count()));
    _log.queue().push(line);
    _heartbeatStage = 3;
    return;
  }

  if (_heartbeatStage == 3) {
    diag::queueMemoryLine(_log.queue(), diag::readMemoryReport(),
                          _faceRenderer.eyesRegion());
    _heartbeatStage = 4;
    return;
  }

  if (_heartbeatStage == 4) {
    queueNetLine();
    _heartbeatStage = 5;
    return;
  }

  if (_heartbeatStage == 5) {
    queueGatewayLine(nowMs);
    _heartbeatStage = 6;
    return;
  }

  if (_heartbeatStage >= 6 && _heartbeatStage <= 9) {
    // Step 6.3: turn and credit counters, one line per iteration.
    if (_heartbeatStage < 8) {
      queueTurnLine(static_cast<uint8_t>(_heartbeatStage - 6));
    } else {
      queueCreditLine(static_cast<uint8_t>(_heartbeatStage - 8));
    }
    ++_heartbeatStage;
    return;
  }

  if (_heartbeatStage == 10) {
    queueActivityLine();
    ++_heartbeatStage;
    return;
  }

  // Aggregate block timings, slowest first. This replaced printing a line per
  // occurrence, which had become the blocking it was measuring.
  diag::BlockStats::reportAndReset(_log.queue());
  _heartbeatStage = 0;

  // Per-interval worst cases, so a single early spike does not mask a later
  // regression.
  _maxLoopSteadyMicros = 0;
  _maxLoopTransitionMicros = 0;
  _maxLoopSpeakingMicros = 0;
  _maxSerialWriteMicros = 0;
  _maxDrainPollMicros = 0;
  _loopCount = 0;
  _faceRenderer.resetStats();
  _log.resetStats();
}

// --- Step 6.3: gateway turns ---------------------------------------------------------

bool App::isTransient(TurnError error) {
  switch (error) {
    case TurnError::ResponseTimeout:
    case TurnError::SessionLost:
    case TurnError::GatewayError:
    case TurnError::Protocol:
      return true;
    case TurnError::None:
    case TurnError::Rejected:
    case TurnError::Injected:
    case TurnError::BufferUnavailable:
      return false;
  }
  return false;
}

bool App::usingGateway() const { return _turnSource == &_gatewayTurn; }

const char* App::sourceName() const {
#ifdef TTH_DIAGNOSTIC_BUILD
  return usingGateway() ? "gateway" : "local mock";
#else
  return "gateway";
#endif
}

void App::syncTurnSourceWithSession(uint32_t nowMs) {
  const bool ready = _session.state() == SessionState::Ready;
  if (ready && !_gatewayTurn.linkReady()) {
    _gatewayTurn.onReady(nowMs);
    return;
  }
  if (!ready && _gatewayTurn.linkReady()) {
    // THE session-loss rule, whatever ended the session (peer close, pong
    // timeout, Wi-Fi, a protocol error): the active turn fails once, here.
    // Its Error event makes App stop capture and playback and release the bus;
    // the session reconnects on its own schedule.
    const GatewayTurnSource::Phase phase = _gatewayTurn.phase();
    _gatewayTurn.onSessionLost(nowMs);
    if (phase != GatewayTurnSource::Phase::Idle) {
      _log.printf("[turn] gateway session ended while %s: turn fails once, robot "
                  "reconnects", toString(phase));
    }
  }
}

void App::handleTurnControl(uint32_t nowMs) {
  const wire::ControlMessage& m = _controlMessage;
  const bool active = m.hasTurn && m.turn != 0 && m.turn == _gatewayTurn.activeTurn();
  switch (m.type) {
    case wire::ControlType::SpeechStart:
      _log.printf("[gw] speech_start turn %lu%s", static_cast<unsigned long>(m.turn),
                  active ? "" : " (not the active turn: dropped, counted)");
      break;
    case wire::ControlType::TurnComplete:
      _log.printf("[gw] turn_complete turn %lu frames=%lu bytes=%lu%s",
                  static_cast<unsigned long>(m.turn), static_cast<unsigned long>(m.frames),
                  static_cast<unsigned long>(m.bytes),
                  active ? "" : " (not the active turn: dropped, counted)");
      break;
    case wire::ControlType::Interrupted:
      _log.printf("[gw] interrupted turn %lu (counted)", static_cast<unsigned long>(m.turn));
      break;
    default:
      break;
  }
  _gatewayTurn.onControl(m, nowMs);
}

void App::serviceErrorHold(uint32_t nowMs) {
  if (!_machine.transientError()) return;
  // Only once the hardware is released: the next state must be truthful.
  if (!_player.isIdle() || _capture.isBusy() || _bargeIn.isActive() || _streamer.isActive()) {
    return;
  }
  const ConversationState previous = _machine.state();
  logStateIfChanged(_machine.tick(nowMs), previous);
}

void App::logMemoryPoint(const char* point) {
  const diag::MemoryReport m = diag::readMemoryReport();
  const DownstreamCredit& credit = _gateway.credit();
  char line[kLogMessageMax];
  snprintf(line, sizeof(line),
           "[mem] M3/M4 %s: internal free=%lu largest=%lu (historical min=%lu) | "
           "ring=%luB credit left=%lu zeroStalls=%lu | tlsAllocFail=%lu",
           point, static_cast<unsigned long>(m.internalFree),
           static_cast<unsigned long>(m.internalLargestBlock),
           static_cast<unsigned long>(m.internalMinFree),
           static_cast<unsigned long>(_gateway.ring().usedBytes()),
           static_cast<unsigned long>(credit.gatewayRemaining()),
           static_cast<unsigned long>(_gateway.stats().zeroCreditStalls),
           static_cast<unsigned long>(_gateway.stats().tlsAllocFailures));
  _log.queue().pushCritical(line);
}

void App::queueTurnLine(uint8_t part) {
  const GatewayTurnCounters& c = _gatewayTurn.counters();
  char line[kLogMessageMax];
  if (part == 0) {
    snprintf(line, sizeof(line),
             "[turn] source=%s phase=%s started=%lu ok=%lu failed=%lu cancelled=%lu "
             "refused=%lu | upBusy=%lu endBusy=%lu outboundHigh=%lu/%lu",
             sourceName(), toString(_gatewayTurn.phase()),
             static_cast<unsigned long>(c.turnsStarted),
             static_cast<unsigned long>(c.turnsCompleted),
             static_cast<unsigned long>(c.turnsFailed),
             static_cast<unsigned long>(c.turnsCancelled),
             static_cast<unsigned long>(c.beginRefused),
             static_cast<unsigned long>(c.upstreamBusy),
             static_cast<unsigned long>(c.turnEndBusy),
             static_cast<unsigned long>(_gateway.outboundHighWater()),
             static_cast<unsigned long>(OutboundQueue::kSlots));
  } else {
    snprintf(line, sizeof(line),
             "[turn] firstResponseTimeouts=%lu sessionLossFailures=%lu protocolFailures=%lu "
             "gatewayErrors=%lu staleEvents=%lu interrupted=%lu firstResponse=%lu/%lums",
             static_cast<unsigned long>(c.firstResponseTimeouts),
             static_cast<unsigned long>(c.sessionLossFailures),
             static_cast<unsigned long>(c.protocolFailures),
             static_cast<unsigned long>(c.gatewayErrors),
             static_cast<unsigned long>(c.staleEvents),
             static_cast<unsigned long>(c.interrupted),
             static_cast<unsigned long>(c.lastFirstResponseMs),
             static_cast<unsigned long>(c.maxFirstResponseMs));
  }
  _log.queue().push(line);
}

void App::queueCreditLine(uint8_t part) {
  const DownstreamCredit& credit = _gateway.credit();
  const GatewayTurnCounters& c = _gatewayTurn.counters();
  const GatewayClient::Stats g = _gateway.stats();
  char line[kLogMessageMax];
  if (part == 0) {
    // Per connection: granted = spent-and-unreturned + left; every spent byte
    // is in the ring or consumed, and consumed = returned + awaiting.
    snprintf(line, sizeof(line),
             "[credit] granted=%lu spent=%lu consumed=%lu returned=%lu left=%lu | "
             "ring used=%lu high=%lu of %lu B | returnedTotal=%lu returnBusy=%lu",
             static_cast<unsigned long>(credit.capacity()),
             static_cast<unsigned long>(credit.received()),
             static_cast<unsigned long>(credit.consumedBytes()),
             static_cast<unsigned long>(credit.returned()),
             static_cast<unsigned long>(credit.gatewayRemaining()),
             static_cast<unsigned long>(_gateway.ring().usedBytes()),
             static_cast<unsigned long>(_gateway.ring().highWaterBytes()),
             static_cast<unsigned long>(TTH_GW_RING_BYTES),
             static_cast<unsigned long>(c.creditReturned),
             static_cast<unsigned long>(c.creditReturnBusy));
  } else {
    snprintf(line, sizeof(line),
             "[credit] zeroStalls=%lu creditViolations=%lu (capacity %lu) staleB=%lu "
             "cancelledB=%lu oldConnB=%lu | sent=%lu sendFail=%lu down=%lu badFrames=%lu",
             static_cast<unsigned long>(g.zeroCreditStalls),
             static_cast<unsigned long>(_gateway.creditViolations()),
             static_cast<unsigned long>(g.capacityViolations),
             static_cast<unsigned long>(c.staleAudioBytes),
             static_cast<unsigned long>(c.cancelledAudioBytes),
             static_cast<unsigned long>(c.oldConnectionBytes),
             static_cast<unsigned long>(g.itemsSent),
             static_cast<unsigned long>(g.sendFailures),
             static_cast<unsigned long>(g.downFrames),
             static_cast<unsigned long>(g.badFrames));
  }
  _log.queue().push(line);
}

// --- on-device activity selection ------------------------------------------------------

ActivityMenuConditions App::activityMenuConditions() const {
  ActivityMenuConditions c;
  c.online = _connectivity == Connectivity::Online;
  c.sessionReady = _session.state() == SessionState::Ready;
  c.conversationReady = _machine.state() == ConversationState::Ready;
  c.audioIdle = _player.isIdle() && !_capture.isBusy();
  c.turnIdle = !_streamer.isActive() && _gatewayTurn.phase() == GatewayTurnSource::Phase::Idle;
  c.bargeInIdle = !_bargeIn.isActive();
  c.catalogReady = _activityCatalog.count() > 0;
  c.usingGateway = usingGateway();
  return c;
}

void App::serviceActivityMenu(uint32_t nowMs) {
  // Short presses ("clicks") of the three touch zones below the display.
  const bool left = M5.BtnA.wasClicked();
  const bool right = M5.BtnC.wasClicked();
  const bool centre = M5.BtnB.wasClicked();
  const ActivityMenuConditions conditions = activityMenuConditions();
  SelectorEvent event = SelectorEvent::None;

  if (!_activitySelector.isOpen()) {
    if (!left) return;
    const char* refusal = activityMenuRefusal(conditions);
    if (refusal != nullptr) {
      _log.printf("[activity] menu refused: %s", refusal);
      if (_haptics.denied(nowMs)) _log.printf("[haptics] refused pattern (2 short pulses)");
      return;
    }
    event = _activitySelector.open(_activityCatalog, nowMs);
  } else {
    const bool linkLost = !conditions.online || !conditions.sessionReady;
    if (linkLost || (_activitySelector.state() == SelectorState::Browsing &&
                     activityMenuRefusal(conditions) != nullptr)) {
      // A pending selection is abandoned too: the saved selection is only
      // changed when the gateway confirms, so the previous one stays.
      event = _activitySelector.cancel();
    } else if (left) {
      event = _activitySelector.previous(_activityCatalog, nowMs);
    } else if (right) {
      event = _activitySelector.next(_activityCatalog, nowMs);
    } else if (centre) {
      event = _activitySelector.confirm(_activityCatalog, nowMs);
    }
  }
  if (event == SelectorEvent::None) event = _activitySelector.tick(nowMs);
  handleSelectorEvent(event, nowMs);
}

void App::handleSelectorEvent(SelectorEvent event, uint32_t nowMs) {
  switch (event) {
    case SelectorEvent::None:
      return;

    case SelectorEvent::Opened:
    case SelectorEvent::Moved:
      markTransitionIteration();
      _log.printf("[activity] menu %s %lu/%lu", event == SelectorEvent::Opened ? "open" : "at",
                  static_cast<unsigned long>(_activitySelector.highlight() + 1),
                  static_cast<unsigned long>(_activityCatalog.count()));
      _activityMenuDirty = true;
      return;

    case SelectorEvent::SelectRequested: {
      markTransitionIteration();
      char text[wire::kMaxControlBytes + 1];
      const size_t n = wire::encodeActivitySelect(text, sizeof(text), _activitySelector.pendingId());
      if (n == 0 || !_gateway.sendText(text, n)) {
        _log.printf("[activity] select could not be queued");
        handleSelectorEvent(_activitySelector.onError(nowMs), nowMs);
        return;
      }
      _log.printf("[activity] select %s sent; waiting for the gateway", _activitySelector.pendingId());
      _activityMenuDirty = true;
      return;
    }

    case SelectorEvent::Failed:
      markTransitionIteration();
      _log.printf("[activity] selection failed: the previous activity stays in use");
      _activityMenuDirty = true;
      return;

    case SelectorEvent::Unchanged:
      _log.printf("[activity] menu closed: kept the current activity");
      closeActivityMenu();
      return;
    case SelectorEvent::Selected:
      closeActivityMenu();
      return;
    case SelectorEvent::TimedOut:
      _log.printf("[activity] menu closed: no input for %lu s",
                  static_cast<unsigned long>(TTH_ACTIVITY_MENU_TIMEOUT_MS / 1000u));
      closeActivityMenu();
      return;
    case SelectorEvent::Dismissed:
      closeActivityMenu();
      return;
    case SelectorEvent::Cancelled:
      _log.printf("[activity] menu closed: robot no longer idle or online");
      closeActivityMenu();
      return;
  }
}

void App::closeActivityMenu() {
  markTransitionIteration();
  _activityMenuDirty = false;
  {
    TTH_TIME_BLOCK("activity menu close");
    _faceRenderer.restoreAfterMenu();
  }
  // The next face frame pushes every region together.
  _faceTransitionPending = true;
}

void App::drawActivityMenu() {
  const uint32_t count = _activityCatalog.count();
  const uint32_t index = _activitySelector.highlight();
  if (count == 0 || index >= count) return;
  char position[16];
  snprintf(position, sizeof(position), "%lu/%lu", static_cast<unsigned long>(index + 1),
           static_cast<unsigned long>(count));
  char title[wire::kMaxActivityTitleBytes + 1];
  toDisplayAscii(_activityCatalog.at(index).title, title, sizeof(title));
  switch (_activitySelector.state()) {
    case SelectorState::Pending:
      _faceRenderer.drawActivityMenu(position, title, "Schimb activitatea...", false);
      return;
    case SelectorState::Failed:
      _faceRenderer.drawActivityMenu(position, title, "Nu s-a putut schimba", false);
      return;
    case SelectorState::Browsing:
    case SelectorState::Closed:
      _faceRenderer.drawActivityMenu(position, title, "", true);
      return;
  }
}

void App::handleActivityList() {
  const ActivityCatalog::Accept result = _activityCatalog.accept(_controlMessage);
  if (result == ActivityCatalog::Accept::Complete) {
    _log.printf("[activity] list: %lu activities, current %ld",
                static_cast<unsigned long>(_activityCatalog.count()),
                static_cast<long>(_activityCatalog.indexOf(_activityCatalog.current()) + 1));
  } else if (result == ActivityCatalog::Accept::Rejected) {
    _log.printf("[activity] list item out of order: list discarded (errors %lu)",
                static_cast<unsigned long>(_activityCatalog.listErrors()));
  }
}

void App::handleActivitySelected(uint32_t nowMs) {
  const char* id = _controlMessage.activity;
  _activityCatalog.setCurrent(id);
  const SelectorEvent event = _activitySelector.onSelected(id, nowMs);
  if (event != SelectorEvent::Selected) {
    _log.printf("[activity] gateway now uses %s", id);
    return;
  }
  // Saved only now that the gateway has switched: a failed selection never
  // replaces the previous one.
  if (_activityPreference.save(id)) {
    memcpy(_savedActivity, id, sizeof(_savedActivity) - 1);
    _savedActivity[sizeof(_savedActivity) - 1] = '\0';
    _log.printf("[activity] selected %s (saved)", id);
  } else {
    _log.printf("[activity] selected %s (NOT saved: NVS write failed)", id);
  }
  handleSelectorEvent(event, nowMs);
}

void App::handleActivitySelectError(uint32_t nowMs) {
  const wire::ControlMessage& m = _controlMessage;
  _log.printf("[activity] gateway refused %s: %.31s",
              m.activity[0] != '\0' ? m.activity : "the selection", m.code);
  if (_activitySelector.state() == SelectorState::Pending) {
    handleSelectorEvent(_activitySelector.onError(nowMs), nowMs);
    return;
  }
  // The saved selection sent in hello was refused. If it no longer exists or
  // is not usable, forget it so the robot stops asking for it; a transient
  // failure keeps it.
  const bool gone = strcmp(m.code, "missing") == 0 || strcmp(m.code, "disabled") == 0 ||
                    strcmp(m.code, "invalid") == 0;
  if (gone && _savedActivity[0] != '\0' &&
      (m.activity[0] == '\0' || strcmp(m.activity, _savedActivity) == 0)) {
    _activityPreference.clear();
    _savedActivity[0] = '\0';
    _log.printf("[activity] saved selection cleared; using the gateway's configured activity");
  }
}

void App::queueActivityLine() {
  char line[kLogMessageMax];
  snprintf(line, sizeof(line),
           "[activity] current=%s saved=%s list=%lu selector=%s selected=%lu failed=%lu "
           "timeouts=%lu listErrors=%lu",
           _activityCatalog.current()[0] != '\0' ? _activityCatalog.current() : "-",
           _savedActivity[0] != '\0' ? _savedActivity : "-",
           static_cast<unsigned long>(_activityCatalog.count()),
           toString(_activitySelector.state()),
           static_cast<unsigned long>(_activitySelector.selections()),
           static_cast<unsigned long>(_activitySelector.failures()),
           static_cast<unsigned long>(_activitySelector.timeouts()),
           static_cast<unsigned long>(_activityCatalog.listErrors()));
  _log.queue().push(line);
}

}  // namespace tth
