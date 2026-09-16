#include "diag/StartupReport.h"

#include <Arduino.h>
#include <M5Unified.h>

#include <stdio.h>

#include "tth/Config.h"

namespace tth {
namespace diag {

namespace {

void rule() {
  Serial.println(
      F("---------------------------------------------------------------"));
}

void heading(const char* text) {
  Serial.println();
  rule();
  Serial.printf("  %s\r\n", text);
  rule();
}

const char* yesNo(bool value) { return value ? "yes" : "NO"; }

}  // namespace

bool printStartupReport(const char* pttInputName) {
  bool healthy = true;

  Serial.println();
  rule();
  Serial.println(F("  TTH Bot - M5Stack Core2 - PHASE 6.3 gateway turns (TLS WebSocket; speech via the gateway)"));
  rule();

  const auto board = M5.getBoard();
  if (board == m5::board_t::board_M5StackCore2) {
    Serial.println(F("board        : board_M5StackCore2"));
  } else {
    Serial.printf("board        : UNEXPECTED (m5::board_t id = %d)\r\n",
                  static_cast<int>(board));
    healthy = false;
  }

  Serial.printf("chip         : %s rev %u, %u MHz\r\n", ESP.getChipModel(),
                static_cast<unsigned>(ESP.getChipRevision()),
                static_cast<unsigned>(ESP.getCpuFreqMHz()));
  Serial.printf("heap         : %u free / %u total\r\n",
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getHeapSize()));

  const uint32_t psram = static_cast<uint32_t>(ESP.getPsramSize());
  Serial.printf("PSRAM        : %u free / %u total\r\n",
                static_cast<unsigned>(ESP.getFreePsram()),
                static_cast<unsigned>(psram));

  // Phase 0 on the real device reported roughly 4 MB usable, not the 8 MB the
  // datasheet suggests, so the design budget is set against the measured
  // figure. The face sprites (~73 KB) and audio buffers must fit inside it.
  if (psram < TTH_PSRAM_MIN_BYTES) {
    Serial.printf(
        "               *** below the %u byte design budget ***\r\n",
        static_cast<unsigned>(TTH_PSRAM_MIN_BYTES));
    healthy = false;
  }

  Serial.printf("battery      : %d%%, charging=%s\r\n",
                M5.Power.getBatteryLevel(),
                M5.Power.isCharging() ? "yes" : "no");

  rule();
  Serial.printf("PTT input    : %s\r\n", pttInputName);
  Serial.printf("mic rate     : %u Hz\r\n",
                static_cast<unsigned>(TTH_MIC_SAMPLE_RATE));
  Serial.printf("speaker rate : %u Hz\r\n",
                static_cast<unsigned>(TTH_SPEAKER_SAMPLE_RATE));
  Serial.printf("debounce     : %u ms (physical button; touch is debounced "
                "in M5Unified)\r\n",
                static_cast<unsigned>(TTH_PTT_DEBOUNCE_MS));
  Serial.printf("max hold     : %u ms\r\n",
                static_cast<unsigned>(TTH_PTT_MAX_HOLD_MS));
  Serial.printf("vibration    : refused action only, %u/%u/%u ms @ level %u "
                "(none when speaking)\r\n",
                static_cast<unsigned>(TTH_DENIED_PULSE_MS),
                static_cast<unsigned>(TTH_DENIED_GAP_MS),
                static_cast<unsigned>(TTH_DENIED_PULSE_MS),
                static_cast<unsigned>(TTH_VIBRATION_LEVEL));
  Serial.printf("turn buffer  : %lu bytes PSRAM (%u s at %u Hz)\r\n",
                static_cast<unsigned long>(TTH_TURN_BUFFER_BYTES),
                static_cast<unsigned>(TTH_CAPTURE_MAX_SECONDS +
                                      TTH_CAPTURE_MARGIN_SECONDS),
                static_cast<unsigned>(TTH_MIC_SAMPLE_RATE));
  Serial.println(F("audio duplex : HALF (strict) - AudioBus arbitrated"));
  rule();

  if (!healthy) {
    Serial.println(F("STARTUP REPORT: PROBLEMS FOUND (see *** lines above)"));
  } else {
    Serial.println(F("STARTUP REPORT: ok"));
  }
  rule();
  Serial.println();

  return healthy;
}

void printFaceGeometry(const FaceGeometry& g) {
  heading("FACE GEOMETRY");
  Serial.printf("screen       : %dx%d, contain scale %.4f\r\n", g.screenWidth,
                g.screenHeight, static_cast<double>(g.scale));
  Serial.printf("eye radius   : %.1f px\r\n", static_cast<double>(g.eyeRadius));
  Serial.printf("eye centres  : x=%d / %d, y=%d\r\n", g.leftEyeCenterX,
                g.rightEyeCenterX, g.eyeCenterY);
  Serial.printf("eyes box     : %dx%d at (%d,%d)  [both eyes, one sprite]\r\n",
                g.eyesBoxW, g.eyesBoxH, g.eyesBoxX, g.eyesBoxY);
  Serial.printf("smile        : %.1fx%.1f px, centre (%d,%d)  [FIXED]\r\n",
                static_cast<double>(g.mouthWidth),
                static_cast<double>(g.mouthHeight), g.mouthCenterX,
                g.mouthCenterY);
  Serial.printf("lower box    : %dx%d at (%d,%d)  [smile + audio bars]\r\n",
                g.lowerBoxW, g.lowerBoxH, g.lowerBoxX, g.lowerBoxY);
  Serial.printf("bars         : %d, half-width %d, centre y %d\r\n", kBarCount,
                g.barHalfWidth, g.barCenterY);
  Serial.printf("bar x        : %d %d %d | %d %d %d\r\n", g.barCenterX[0],
                g.barCenterX[1], g.barCenterX[2], g.barCenterX[3],
                g.barCenterX[4], g.barCenterX[5]);
  Serial.printf("bar max half : %d %d %d (outer to inner)\r\n",
                g.barMaxHalfHeight[0], g.barMaxHalfHeight[1],
                g.barMaxHalfHeight[2]);
  Serial.printf("sprite PSRAM : %d bytes (16bpp, 2 sprites)\r\n",
                g.spriteBytes());
  // At 40 MHz and 16 bits per pixel the panel takes 0.4 us per pixel, so this
  // is the SPI floor for a full-face state transition.
  Serial.printf("full face    : %d px -> %d us of SPI at 40 MHz\r\n",
                g.fullFacePixels(), (g.fullFacePixels() * 2 * 8) / 40);
  Serial.printf("fits on screen: %s, boxes disjoint: %s\r\n",
                yesNo(g.fitsOnScreen()), yesNo(g.boxesAreDisjoint()));
  if (!g.fitsOnScreen() || !g.boxesAreDisjoint()) {
    Serial.println(F("               *** face layout is invalid ***"));
  }
  rule();
}

namespace {

// The caps that matter for real allocations. MALLOC_CAP_INTERNAL alone would
// include IRAM, which no data buffer can use -- see tth/MemorySafety.h and the
// LoadStoreError it caused.
const uint32_t kInternalDataCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

}  // namespace

MemoryReport readMemoryReport() {
  MemoryReport report;
  report.internalFree =
      static_cast<uint32_t>(heap_caps_get_free_size(kInternalDataCaps));
  report.internalLargestBlock = static_cast<uint32_t>(
      heap_caps_get_largest_free_block(kInternalDataCaps));
  report.internalMinFree = static_cast<uint32_t>(
      heap_caps_get_minimum_free_size(kInternalDataCaps));
  report.psramFree =
      static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  report.psramLargestBlock = static_cast<uint32_t>(
      heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  return report;
}

void printMemoryReport(const MemoryReport& report, const char* eyesRegion,
                       const char* lowerFaceRegion, uint32_t spriteBytes,
                       uint32_t turnBufferBytes) {
  heading("MEMORY");
  Serial.printf("internal free   : %lu bytes\r\n",
                static_cast<unsigned long>(report.internalFree));
  Serial.printf("largest block   : %lu bytes\r\n",
                static_cast<unsigned long>(report.internalLargestBlock));
  if (report.internalFree > 0) {
    Serial.printf(
        "contiguous      : %lu%% of free internal\r\n",
        static_cast<unsigned long>((report.internalLargestBlock * 100ul) /
                                   report.internalFree));
  }
  Serial.printf("internal minimum: %lu bytes (low-water since boot)\r\n",
                static_cast<unsigned long>(report.internalMinFree));
  Serial.printf("PSRAM free      : %lu bytes (largest %lu)\r\n",
                static_cast<unsigned long>(report.psramFree),
                static_cast<unsigned long>(report.psramLargestBlock));
  rule();
  Serial.printf("face sprites    : %lu bytes, eyes in %s, lower face in %s\r\n",
                static_cast<unsigned long>(spriteBytes), eyesRegion,
                lowerFaceRegion);
  Serial.printf("turn buffer     : %lu bytes in PSRAM\r\n",
                static_cast<unsigned long>(turnBufferBytes));
  Serial.println(F("DECIDED (Step 6.2 memory gate): face sprites in PSRAM at"));
  Serial.println(F("  compile time, no fallback. Internal RAM is kept for"));
  Serial.println(F("  Wi-Fi, TLS and audio. The loop warning threshold is"));
  Serial.println(F("  unchanged; watch maxPush / maxTrans for the render cost."));
  rule();
}

// Heartbeat lines are QUEUED, never written directly.
//
// Writing them synchronously cost 21-31 ms per heartbeat at 115200 baud, which
// is most of a 31-57 ms loop iteration. They are also split across separate
// loop iterations by the caller, so no single iteration formats more than one.

void queueHeartbeat(LogQueue& queue, uint32_t uptimeMs,
                    uint32_t maxLoopSteadyMicros,
                    uint32_t maxLoopTransitionMicros,
                    uint32_t maxSerialWriteMicros, uint32_t loopCount,
                    const char* conversationState, const char* audioOwner,
                    const char* faceState, uint32_t logDrops,
                    uint32_t criticalLogDrops) {
  char line[kLogMessageMax];
  snprintf(line, sizeof(line),
           "[hb] up=%lus state=%s face=%s audio=%s loops=%lu "
           "maxLoopSteady=%luus maxLoopTransition=%luus "
           "maxSerialWrite=%luus logDrops=%lu criticalDrops=%lu",
           static_cast<unsigned long>(uptimeMs / 1000), conversationState,
           faceState, audioOwner, static_cast<unsigned long>(loopCount),
           static_cast<unsigned long>(maxLoopSteadyMicros),
           static_cast<unsigned long>(maxLoopTransitionMicros),
           static_cast<unsigned long>(maxSerialWriteMicros),
           static_cast<unsigned long>(logDrops),
           static_cast<unsigned long>(criticalLogDrops));
  queue.push(line);

  // Only STEADY-STATE work is held to the budget. A turn transition pays a
  // known, documented one-off audio-frame priming cost; warning about that
  // every press would train the reader to ignore the warning.
  if (maxLoopSteadyMicros > TTH_LOOP_WARN_MICROS) {
    snprintf(line, sizeof(line),
             "[hb] *** steady-state loop exceeded %lu us ***",
             static_cast<unsigned long>(TTH_LOOP_WARN_MICROS));
    queue.push(line);
  }
}

void queueCaptureLine(LogQueue& queue, uint32_t maxFacePushMicros,
                      uint32_t maxTransitionMicros, uint32_t captureBytes,
                      uint32_t sessionHighWaterBytes, uint32_t capacityBytes,
                      uint32_t maxReadMicros, uint32_t lastDrainTotalMs,
                      uint32_t maxDrainTotalMs, uint32_t maxDrainPollMicros) {
  char line[kLogMessageMax];
  snprintf(line, sizeof(line),
           "[cap] capture=%luB sessionHighWater=%luB capacity=%luB "
           "maxRead=%luus drainTotal=%lums/%lums maxDrainPoll=%luus "
           "maxPush=%luus maxTrans=%luus",
           static_cast<unsigned long>(captureBytes),
           static_cast<unsigned long>(sessionHighWaterBytes),
           static_cast<unsigned long>(capacityBytes),
           static_cast<unsigned long>(maxReadMicros),
           static_cast<unsigned long>(lastDrainTotalMs),
           static_cast<unsigned long>(maxDrainTotalMs),
           static_cast<unsigned long>(maxDrainPollMicros),
           static_cast<unsigned long>(maxFacePushMicros),
           static_cast<unsigned long>(maxTransitionMicros));
  queue.push(line);
}

void queueMemoryLine(LogQueue& queue, const MemoryReport& report,
                     const char* spriteRegion) {
  char line[kLogMessageMax];
  snprintf(line, sizeof(line),
           "[mem] internal free=%lu largest=%lu min=%lu | psram free=%lu "
           "largest=%lu | sprites=%s",
           static_cast<unsigned long>(report.internalFree),
           static_cast<unsigned long>(report.internalLargestBlock),
           static_cast<unsigned long>(report.internalMinFree),
           static_cast<unsigned long>(report.psramFree),
           static_cast<unsigned long>(report.psramLargestBlock), spriteRegion);
  queue.push(line);
}

}  // namespace diag
}  // namespace tth
