#include "ui/FaceRenderer.h"

#include <Arduino.h>
#include <math.h>

#include "tth/Config.h"
#include "tth/FaceAnimator.h"

namespace tth {

namespace {

// Anything smaller than this is invisible at 320x240, so it is not worth a
// redraw and a multi-millisecond SPI push.
const float kParamEpsilon = 0.004f;

bool differs(float a, float b) { return fabsf(a - b) > kParamEpsilon; }

uint16_t toRgb565(uint32_t rgb888) {
  const uint8_t r = static_cast<uint8_t>((rgb888 >> 16) & 0xFF);
  const uint8_t g = static_cast<uint8_t>((rgb888 >> 8) & 0xFF);
  const uint8_t b = static_cast<uint8_t>(rgb888 & 0xFF);
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) |
                               (b >> 3));
}

// Blends two 24-bit colours and converts to RGB565. The panel has no alpha
// channel, so a bar fading out is drawn in a colour mixed towards the
// background -- which is what makes it disappear gradually rather than
// blinking off.
uint16_t blendRgb565(uint32_t from, uint32_t to, float t) {
  if (t <= 0.0f) return toRgb565(from);
  if (t >= 1.0f) return toRgb565(to);
  const float inv = 1.0f - t;
  const uint32_t r = static_cast<uint32_t>(
      (((from >> 16) & 0xFF) * inv) + (((to >> 16) & 0xFF) * t));
  const uint32_t g = static_cast<uint32_t>(
      (((from >> 8) & 0xFF) * inv) + (((to >> 8) & 0xFF) * t));
  const uint32_t b =
      static_cast<uint32_t>(((from & 0xFF) * inv) + ((to & 0xFF) * t));
  return toRgb565((r << 16) | (g << 8) | b);
}

}  // namespace

bool FaceRenderer::allocateSprite(M5Canvas& canvas, int width, int height,
                                  const char* name, SpriteMemory memory) {
  canvas.setColorDepth(16);

  // Exactly the configured region, nothing else: no retry elsewhere.
  canvas.setPsram(memory == SpriteMemory::Psram);
  if (canvas.createSprite(width, height) == nullptr) {
    Serial.printf("[face] *** %s sprite %dx%d (%d bytes) could not be allocated "
                  "in %s; face disabled (no fallback) ***\r\n",
                  name, width, height, width * height * 2, toString(memory));
    return false;
  }

  const uintptr_t address = reinterpret_cast<uintptr_t>(canvas.getBuffer());
  if (!spriteInRequestedRegion(memory, address)) {
    Serial.printf("[face] *** %s sprite landed in %s at 0x%08lx, not %s; face "
                  "disabled (no fallback) ***\r\n",
                  name, memoryRegionName(address),
                  static_cast<unsigned long>(address), toString(memory));
    canvas.deleteSprite();
    return false;
  }

  Serial.printf("[face] %s sprite %dx%d (%d bytes) in %s at 0x%08lx\r\n", name,
                width, height, width * height * 2, toString(memory),
                static_cast<unsigned long>(address));
  return true;
}

FaceRenderer::FaceRenderer()
    : _colorBackground(0),
      _colorPrimary(0),
      _colorDim(0),
      _eyesDirty(true),
      _lowerDirty(true),
      _ready(false),
      _pushCount(0),
      _maxPushMicros(0),
      _maxTransitionMicros(0) {
  // Deliberately impossible values so the first render() always draws.
  _renderedEyes.eyeScale = -1.0f;
  _renderedEyes.eyeOpenness = -1.0f;
  _renderedEyes.pupilDx = -99.0f;
  _renderedEyes.pupilDy = -99.0f;
  _renderedEyes.speechLevel = -1.0f;
  _renderedEyes.dim = false;
  _renderedLower = _renderedEyes;
}

bool FaceRenderer::begin(const FaceGeometry& geometry) {
  _geometry = geometry;

  _colorBackground = toRgb565(TTH_COLOR_BACKGROUND);
  _colorPrimary = toRgb565(TTH_COLOR_CYAN);
  _colorDim = toRgb565(TTH_COLOR_CYAN_DIM);

  M5.Display.fillScreen(_colorBackground);

  // PSRAM, BY DECISION (Step 6.2 memory gate, PHASE6_PLAN §7).
  //
  // History: the sprites started in PSRAM, where a full-face transition took
  // 25.5 ms against a 16.2 ms SPI floor -- the extra ~9.3 ms is PSRAM
  // bandwidth, because a transition touches ~162 KB of it (60 KB + 21 KB
  // cleared, 81 KB read back for the push) at roughly 17 MB/s. They were moved
  // to internal DRAM for speed.
  //
  // With Wi-Fi and a TLS session up, that 81 KB is what internal RAM cannot
  // spare: the first physical Step 6.2 run measured 28 472 B current internal
  // free (< 32 KB) and a 25 588 B largest block during playback. So the
  // sprites go back to PSRAM, deterministically. The rendering cost returns
  // with them; it is measured and reported, not hidden.
  //
  // The 1.4 MB turn buffer was always in PSRAM.
  const SpriteMemory eyesMemory =
      TTH_FACE_EYES_IN_PSRAM ? SpriteMemory::Psram : SpriteMemory::InternalDram;
  const SpriteMemory lowerMemory = TTH_FACE_LOWER_FACE_IN_PSRAM
                                       ? SpriteMemory::Psram
                                       : SpriteMemory::InternalDram;
  if (!allocateSprite(_eyes, geometry.eyesBoxW, geometry.eyesBoxH, "eyes",
                      eyesMemory)) {
    return false;
  }
  if (!allocateSprite(_lowerFace, geometry.lowerBoxW, geometry.lowerBoxH,
                      "lower face", lowerMemory)) {
    return false;
  }

  _eyes.fillSprite(_colorBackground);
  _lowerFace.fillSprite(_colorBackground);

  _ready = true;
  invalidateAll();
  return true;
}

const char* FaceRenderer::eyesRegion() const {
  return memoryRegionName(reinterpret_cast<uintptr_t>(_eyes.getBuffer()));
}

const char* FaceRenderer::lowerFaceRegion() const {
  return memoryRegionName(reinterpret_cast<uintptr_t>(_lowerFace.getBuffer()));
}

uint32_t FaceRenderer::spriteBytes() const {
  return static_cast<uint32_t>(_geometry.spriteBytes());
}

void FaceRenderer::invalidateAll() {
  _eyesDirty = true;
  _lowerDirty = true;
}

void FaceRenderer::drawActivityMenu(const char* position, const char* title,
                                    const char* bottom, bool bottomIsHint) {
  const int w = M5.Display.width();
  const int h = M5.Display.height();
  M5.Display.startWrite();
  M5.Display.fillScreen(_colorBackground);
  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.setTextColor(_colorDim, _colorBackground);
  M5.Display.setFont(&fonts::Font4);
  M5.Display.drawString(position, w / 2, 30);

  // The title: one line of Font4 if it fits; otherwise split at a space into
  // two lines; otherwise the smaller Font2.
  M5.Display.setTextColor(_colorPrimary, _colorBackground);
  const int maxWidth = w - 16;
  if (M5.Display.textWidth(title) <= maxWidth) {
    M5.Display.drawString(title, w / 2, h / 2 - 10);
  } else {
    char first[64];
    char second[64];
    const size_t length = strlen(title);
    size_t split = 0;
    for (size_t i = 0; i < length && i < sizeof(first) - 1; ++i) {
      if (title[i] == ' ' && i <= length / 2 + 4) split = i;
    }
    if (split == 0) split = length / 2;
    snprintf(first, sizeof(first), "%.*s", static_cast<int>(split), title);
    snprintf(second, sizeof(second), "%s", title + split + (title[split] == ' ' ? 1 : 0));
    if (M5.Display.textWidth(first) > maxWidth || M5.Display.textWidth(second) > maxWidth) {
      M5.Display.setFont(&fonts::Font2);
    }
    M5.Display.drawString(first, w / 2, h / 2 - 26);
    M5.Display.drawString(second, w / 2, h / 2 + 6);
  }

  // Bottom line: arrows and OK above the three touch zones, or a status.
  M5.Display.setFont(&fonts::Font4);
  if (bottomIsHint) {
    M5.Display.setTextColor(_colorDim, _colorBackground);
    M5.Display.drawString("<", w / 6, h - 22);
    M5.Display.drawString("OK", w / 2, h - 22);
    M5.Display.drawString(">", (5 * w) / 6, h - 22);
  } else {
    M5.Display.setTextColor(_colorPrimary, _colorBackground);
    M5.Display.setFont(&fonts::Font2);
    M5.Display.drawString(bottom, w / 2, h - 22);
  }
  M5.Display.endWrite();
}

void FaceRenderer::restoreAfterMenu() {
  M5.Display.startWrite();
  M5.Display.fillScreen(_colorBackground);
  M5.Display.endWrite();
  invalidateAll();
}

bool FaceRenderer::eyeParamsChanged(const FaceFrame& frame) const {
  return differs(frame.eyeScale, _renderedEyes.eyeScale) ||
         differs(frame.eyeOpenness, _renderedEyes.eyeOpenness) ||
         differs(frame.pupilDx, _renderedEyes.pupilDx) ||
         differs(frame.pupilDy, _renderedEyes.pupilDy) ||
         frame.dim != _renderedEyes.dim;
}

bool FaceRenderer::lowerParamsChanged(const FaceFrame& frame) const {
  // The smile is a constant, so the lower face only ever changes because the
  // bars moved or the face switched to or from the dim error colour. Most
  // state transitions therefore do not touch this sprite at all.
  return differs(frame.speechLevel, _renderedLower.speechLevel) ||
         frame.dim != _renderedLower.dim;
}

void FaceRenderer::drawEyes(const FaceFrame& frame) {
  _eyes.fillSprite(_colorBackground);

  const uint16_t color = frame.dim ? _colorDim : _colorPrimary;

  // BOTH eyes resolved from the SAME frame in one call. There is no code path
  // here that could give the left and right eye different blink phases.
  const EyePairRender pair = eyeRenderFor(_geometry, frame);
  const EyeRender eyes[2] = {pair.left, pair.right};

  for (int i = 0; i < 2; ++i) {
    const EyeRender& eye = eyes[i];
    _eyes.fillEllipse(eye.centerX, eye.centerY, eye.radiusX, eye.radiusY,
                      color);
    if (eye.pupilVisible) {
      // The pupil is a hole punched in the eye, not a third colour, so the
      // face keeps its two-colour identity.
      _eyes.fillEllipse(eye.pupilCenterX, eye.pupilCenterY, eye.pupilRadiusX,
                        eye.pupilRadiusY, _colorBackground);
    }
  }
}

void FaceRenderer::drawFixedSmile(uint16_t color) {
  // THE SMILE NEVER CHANGES. It takes no amplitude, no openness and no scale:
  // the same small friendly closed smile in READY, LISTENING, WAITING,
  // SPEAKING and ERROR. Only the colour differs, and only for the error face.
  //
  // An animated open mouth was tried and rejected -- on the real device it
  // read as a frightened grimace. Speech is shown by the bars instead.
  const float halfWidth = _geometry.mouthWidth * 0.5f;
  const float curve = _geometry.mouthHeight * face::kMouthCurveRatio;
  const float half = 0.5f * _geometry.mouthHeight * face::kSmileThicknessRatio;

  const int cx = _geometry.mouthCenterX - _geometry.lowerBoxX;
  const int cy = _geometry.mouthCenterY - _geometry.lowerBoxY;
  const int halfWidthPx = static_cast<int>(halfWidth);

  for (int dx = -halfWidthPx; dx <= halfWidthPx; ++dx) {
    const float u = static_cast<float>(dx) / halfWidth;

    // A fixed smile bow, thickest in the middle and tapering to nothing at the
    // corners along a circular cap, so the ends are rounded rather than
    // pointed.
    const float centreY = static_cast<float>(cy) + (curve * (1.0f - (u * u)));

    const float distFromEnd = halfWidth - fabsf(static_cast<float>(dx));
    float thickness = half;
    if (distFromEnd < half) {
      const float k = half - distFromEnd;
      thickness = sqrtf((half * half) - (k * k));
    }
    if (thickness < 0.5f) continue;

    const int top = static_cast<int>(centreY - thickness + 0.5f);
    const int bottom = static_cast<int>(centreY + thickness + 0.5f);
    _lowerFace.drawFastVLine(cx + dx, top, (bottom - top) + 1, color);
  }
}

void FaceRenderer::drawLowerFace(const FaceFrame& frame) {
  _lowerFace.fillSprite(_colorBackground);

  const uint16_t smileColor = frame.dim ? _colorDim : _colorPrimary;
  drawFixedSmile(smileColor);

  // Audio level bars, three either side, at fixed horizontal positions. Only
  // their height and intensity animate; nothing about the face itself moves.
  BarRender bars[kBarCount];
  audioBarsFor(_geometry, frame, bars);

  const uint32_t barTarget = frame.dim ? TTH_COLOR_CYAN_DIM : TTH_COLOR_CYAN;

  for (int i = 0; i < kBarCount; ++i) {
    const BarRender& bar = bars[i];
    if (!bar.visible) continue;

    const uint16_t color =
        blendRgb565(TTH_COLOR_BACKGROUND, barTarget, bar.intensity);

    const int width = (2 * bar.halfWidth) + 1;
    const int height = (2 * bar.halfHeight) + 1;
    // Radius equal to the half width makes a stadium: fully rounded ends, no
    // sharp rectangles at any height.
    _lowerFace.fillRoundRect(bar.centerX - bar.halfWidth,
                             bar.centerY - bar.halfHeight, width, height,
                             bar.halfWidth, color);
  }
}

void FaceRenderer::pushCanvas(M5Canvas& canvas, int x, int y) {
  const uint32_t start = micros();
  canvas.pushSprite(&M5.Display, x, y);
  const uint32_t elapsed = micros() - start;
  if (elapsed > _maxPushMicros) _maxPushMicros = elapsed;
  ++_pushCount;
}

void FaceRenderer::render(const FaceFrame& frame, bool atomic) {
  if (!_ready) return;

  if (eyeParamsChanged(frame)) {
    _eyesDirty = true;
    _renderedEyes = frame;
  }
  if (lowerParamsChanged(frame)) {
    _lowerDirty = true;
    _renderedLower = frame;
  }

  if (!_eyesDirty && !_lowerDirty) return;

  const uint32_t start = micros();

  // Draw EVERYTHING that changed before pushing anything. Drawing is CPU work
  // into PSRAM; pushing is the SPI transfer. Doing all the drawing first keeps
  // the display transaction as short as possible.
  if (_eyesDirty) drawEyes(_renderedEyes);
  if (_lowerDirty) drawLowerFace(_renderedLower);

  // One transaction for every region. On a state transition that means the
  // eyes and the lower face reach the panel back to back with no gap, so the
  // face changes as one.
  M5.Display.startWrite();
  if (_eyesDirty) {
    pushCanvas(_eyes, _geometry.eyesBoxX, _geometry.eyesBoxY);
    _eyesDirty = false;
  }
  if (_lowerDirty) {
    pushCanvas(_lowerFace, _geometry.lowerBoxX, _geometry.lowerBoxY);
    _lowerDirty = false;
  }
  M5.Display.endWrite();

  if (atomic) {
    const uint32_t elapsed = micros() - start;
    if (elapsed > _maxTransitionMicros) _maxTransitionMicros = elapsed;
  }
}

}  // namespace tth
