#pragma once

#include <stdint.h>

#include "audio/M5AudioDevice.h"
#include "audio/M5MicrophoneCapture.h"
#include "audio/M5SpeakerOutput.h"
#include "diag/SerialLog.h"
#include "haptics/Haptics.h"
#include "net/EspRandom.h"
#include "net/GatewayClient.h"
#include "net/MbedCertificateCheck.h"
#include "net/NvsConfigStorage.h"
#include "net/WifiLink.h"
#include "tth/GatewayProtocol.h"
#include "tth/GatewaySession.h"
#include "tth/GatewayTurnSource.h"
#include "tth/LowWaterTracker.h"
#include "tth/TrustAnchor.h"
#include "tth/ActivitySelector.h"
#include "tth/AppState.h"
#include "tth/AudioBus.h"
#include "tth/AudioFormat.h"
#include "tth/BargeIn.h"
#include "tth/CaptureController.h"
#include "tth/CaptureSummary.h"
#include "tth/Config.h"
#include "tth/ConversationStateMachine.h"
#include "tth/ConfigStore.h"
#include "tth/Provisioning.h"
#include "tth/SerialLineAssembler.h"
#include "tth/FaceAnimator.h"
#include "tth/FaceGeometry.h"
#include "tth/FaceOverride.h"
#include "tth/FaceState.h"
#include "tth/IPushToTalkInput.h"
#ifdef TTH_DIAGNOSTIC_BUILD
#include "tth/LocalMockTurnSource.h"
#endif
#include "tth/MemorySafety.h"
#include "tth/PcmPlayer.h"
#include "tth/TurnReportGate.h"
#include "tth/TurnStreamer.h"
#include "ui/FaceRenderer.h"

namespace tth {

// The firmware application object.
//
// Step 6.3 scope: everything from Phases 1-5 and Steps 6.1-6.2, with the turn
// source now the GATEWAY: the child's speech streams to the gateway, and the
// response audio streams back into the downstream ring and the player.
// Production always uses GatewayTurnSource. The Phase 5 local mock exists
// only in a diagnostic build (-DTTH_DIAGNOSTIC_BUILD=1).
//
// App is the ONLY thing that touches the state machine, the face, AudioBus
// and the player. The turn source never calls in: its events and audio are
// pulled here, in the cooperative loop. The network task never calls App.
//
// The loop contract every component honours: poll(), return promptly, never
// call delay(). The heartbeat's maxLoop figures are what prove it.
class App : private IBargeInOps {
 public:
  App();

  // Called once from setup().
  void begin();

  // Called from loop(). Must return promptly.
  void tick();

  ConversationState state() const { return _machine.state(); }

 private:
  bool allocateAudioBuffers();
  void servicePushToTalk(uint32_t nowMs);
  void handlePress(uint32_t nowMs);
  void serviceCapture(uint32_t nowMs);
  void serviceStream(uint32_t nowMs);
  void serviceTurnSource(uint32_t nowMs);
  void handleTurnEvent(const TurnEvent& event, uint32_t nowMs);
  void pumpPlayback(uint32_t nowMs);
  void servicePlayback(uint32_t nowMs);
  void serviceBargeIn(uint32_t nowMs);
  void beginTurn(uint32_t nowMs);
  bool startCaptureTurn(uint32_t nowMs);
  void finishTurn(uint32_t nowMs, CaptureStopReason reason);
  void startPlayback(uint32_t nowMs);
  void finishPlayback(uint32_t nowMs);
  // `transient`: a gateway failure (D4) shows ERROR for TTH_TURN_ERROR_HOLD_MS
  // and then returns to the rest state; a local failure stays until a press.
  void failTurn(uint32_t nowMs, const char* why, bool transient);
  void printCaptureSummary();
  void printStreamSummary(const char* outcome);
  void printPlaybackSummary();
  void serviceFace(uint32_t nowMs);
  void serviceSerialCommands(uint32_t nowMs);
  void serviceHeartbeat(uint32_t nowMs);
  void logStateIfChanged(bool changed, ConversationState previous);
  void markTransitionIteration() { _transitionIteration = true; }
  void printFaceCommandHelp();

  // --- Step 6.1: provisioning and Wi-Fi ---------------------------------
  void serviceNetwork(uint32_t nowMs);
  void handleDiagnosticKey(char key, uint32_t nowMs);
  void handleProvisioningLine(const char* line, size_t length, uint32_t nowMs);
  bool provisioningWritesAllowed() const;
  void queueNetLine();

  // --- Step 6.2: the gateway session ---------------------------------------
  bool allocateTrustAnchorBuffers();
  void applyGatewayTarget(uint32_t nowMs);
  void handleGatewayEvent(uint32_t nowMs);
  void executeSessionAction(SessionAction action, uint32_t nowMs);
  void logSessionChange(SessionState previous, uint32_t nowMs);
  Connectivity computeConnectivity() const;
  void updateConnectivity();
  void queueGatewayLine(uint32_t nowMs);
  // Step 6.2 memory investigation: logs a new internal low-water mark with the
  // loop stage that first observed it.
  void checkLowWater(const char* stage, uint32_t nowMs);

  // --- Step 6.3: gateway turns ---------------------------------------------
  // The one rule for a session that ends: the turn source fails the active
  // turn once; App then stops capture and playback and releases the bus.
  void syncTurnSourceWithSession(uint32_t nowMs);
  void handleTurnControl(uint32_t nowMs);
  // A transient ERROR ends once its hold has passed and the audio is released.
  void serviceErrorHold(uint32_t nowMs);
  // Two lines each, on separate loop iterations (part 0 or 1).
  void queueTurnLine(uint8_t part);
  void queueCreditLine(uint8_t part);
  // M3/M4 (PHASE6_PLAN §7): current internal free and largest block at a named
  // point of a turn. The historical minimum is printed separately.
  void logMemoryPoint(const char* point);
  static bool isTransient(TurnError error);
  const char* sourceName() const;
  bool usingGateway() const;

  // --- On-device activity selection -------------------------------------------
  void serviceActivityMenu(uint32_t nowMs);
  void handleSelectorEvent(SelectorEvent event, uint32_t nowMs);
  ActivityMenuConditions activityMenuConditions() const;
  void drawActivityMenu();
  void closeActivityMenu();
  void handleActivityList();
  void handleActivitySelected(uint32_t nowMs);
  void handleActivitySelectError(uint32_t nowMs);
  void queueActivityLine();

  // --- IBargeInOps: the steps BargeIn sequences, in its order -------------
  void stopAcceptingPlayback() override;
  void stopSpeaker(uint32_t nowMs) override;
  bool speakerQuiet() override;
  void cancelTurnSource() override;
  void releaseSpeaker() override;
  bool startCapture(uint32_t nowMs) override;
  bool stillHeld() const override;

  M5AudioDevice _audioDevice;
  AudioBus _audioBus;
  M5MicrophoneCapture _microphone;
  CaptureController _capture;
  // Preallocated once at start-up, never resized. See allocateAudioBuffers().
  int16_t* _turnStorage;
  int16_t* _chunkStorage;
  // Two DMA-capable DRAM buffers the microphone task writes into directly.
  int16_t* _micChunk[2];
  uint32_t _lastCaptureStatusMs;
  diag::SerialLog& _log;
  TurnReportGate _reportGate;
  uint32_t _maxDrainPollMicros;
  // Heartbeat lines are emitted on SEPARATE loop iterations so one iteration
  // never carries two long lines.
  uint8_t _heartbeatStage;
  IPushToTalkInput& _ptt;
  ConversationStateMachine _machine;

  // --- Phase 5 ---------------------------------------------------------------
  const AudioFormat _captureFormat;
  M5SpeakerOutput _speakerOutput;
  PcmPlayer _player;
  // Three DMA-capable DRAM slots the speaker task reads from directly.
  int16_t* _playbackSlot[PcmPlayer::kSlots];
  TurnStreamer _streamer;
#ifdef TTH_DIAGNOSTIC_BUILD
  // One chunk of synthetic speech, generated on demand (diagnostic mock only).
  int16_t* _synthScratch;
  LocalMockTurnSource _mockTurn;
  // Diagnostic build only: 't' selects the local mock instead of the gateway.
  bool _useMock;
#endif
  Haptics _haptics;
  BargeIn _bargeIn;
  // Cleared by barge-in step 1 and by any failure: no further response audio
  // is pulled from the turn source.
  bool _acceptPlayback;
  // SpeechStart arrived, but the player is opened only once the capture has
  // fully released the microphone.
  bool _speechStartPending;
  AudioFormat _pendingSpeechFormat;
  uint32_t _speechStartMs;
  uint32_t _maxLoopSpeakingMicros;
  // The speaker master volume the current/last response played at.
  uint8_t _streamVolume;

  FaceGeometry _faceGeometry;
  FaceRenderer _faceRenderer;
  FaceAnimator _faceAnimator;
  SpeechLevelSmoother _speechSmoother;
  // Diagnostic: index into kFixedSpeechLevels, or kFixedLevelCount to use the
  // simulated envelope. Stepped by the 'b' key.
  int _fixedLevelIndex;
  FaceOverride _faceOverride;
  FaceState _shownFace;
  uint32_t _faceEnteredMs;
  uint32_t _lastFaceFrameMs;
  // Set for exactly one frame after the face state changes, so the renderer
  // knows to push every region together instead of incrementally.
  bool _faceTransitionPending;

  uint32_t _bootMs;
  uint32_t _lastHeartbeatMs;
  // Two figures, because they answer different questions: steady state is the
  // budget audio servicing lives in; a transition is the one-off cost of
  // starting or ending a turn.
  uint32_t _maxLoopSteadyMicros;
  uint32_t _maxLoopTransitionMicros;
  bool _transitionIteration;
  uint32_t _maxSerialWriteMicros;
  uint32_t _loopCount;

  // Periodic "still holding" reporting, so a 45 second maximum-hold test is
  // observable on the serial monitor instead of looking like a hang.
  uint32_t _lastHoldTickMs;

  // --- Step 6.1 -----------------------------------------------------------
  // Persistent configuration: NVS namespace "tth" (two A/B record slots),
  // the store that commits to it transactionally, and the USB-serial
  // provisioning session that edits it. Credentials live ONLY in NVS -- never
  // in the firmware image -- and are never printed.
  NvsConfigStorage _nvs;
  ConfigStore _configStore;
  EspRandom _random;
  ProvisioningSession _provisioning;
  // Splits serial input into single diagnostic keys and '!'-prefixed lines.
  SerialLineAssembler _serialLines;
  WifiLink _wifi;

  // --- Step 6.2 -----------------------------------------------------------
  // The gateway CA (a public certificate), stored transactionally like the
  // configuration, in PSRAM buffers allocated at boot.
  TrustAnchorStore _trustStore;
  char* _caActive;
  uint8_t* _caScratchA;
  uint8_t* _caScratchB;
  char* _caStaging;
  MbedCertificateCheck _certificateCheck;
  // The TLS WebSocket transport (its own task on core 0) and the portable
  // session state machine that decides what it does.
  GatewayClient _gateway;
  GatewaySession _session;
  // Tags each connect; events from an older connection are dropped.
  uint32_t _gatewayGeneration;
  uint32_t _staleGatewayEvents;
  // Wi-Fi and gateway combined: what the conversation and the face see.
  Connectivity _connectivity;
  bool _m2Reported;
  // Scratch, so neither a 548-byte event nor a parsed message sits on the stack.
  GatewayClient::Event _gatewayEvent;
  wire::ControlMessage _controlMessage;
  LowWaterTracker _lowWater;

  // --- Step 6.3 -----------------------------------------------------------
  // Declared after _gateway: it holds references to its ring and credit.
  GatewayTurnSource _gatewayTurn;
  // The source this turn uses: always _gatewayTurn in production.
  ITurnSource* _turnSource;
  // Zero-credit stalls already reported as an M4 memory point.
  uint32_t _reportedZeroStalls;
  uint32_t _lastZeroStallReportMs;
  // The last startCaptureTurn() failure was the gateway refusing the turn.
  bool _startFailureTransient;

  // --- On-device activity selection --------------------------------------------
  // Metadata only (ids, bounded titles, modes): never a prompt.
  ActivityCatalog _activityCatalog;
  ActivitySelector _activitySelector;
  ActivityPreference _activityPreference;
  // The saved selection, sent in hello; "" = the gateway's configured default.
  char _savedActivity[wire::kActivityIdChars + 1];
  bool _activityMenuDirty;
};

}  // namespace tth
