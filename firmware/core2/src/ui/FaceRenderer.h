#pragma once

#include <M5Unified.h>
#include <stdint.h>

#include "tth/FaceFrame.h"
#include "tth/FaceGeometry.h"
#include "tth/MemorySafety.h"
#include "tth/SpritePlacement.h"

namespace tth {

// Draws the robot face: two eyes and one mouth on a dark ground. The only
// other thing that ever reaches the display is the activity menu
// (drawActivityMenu), shown full screen while the child chooses an activity;
// restoreAfterMenu() puts the face back exactly.
//
// TWO SPRITES, NOT A FRAMEBUFFER
//
// A full-screen 320x240 16bpp buffer would be 150 KB and would have to be
// pushed whole for any change. Instead each region owns a sprite, sized to the
// largest extent the animator can produce, and is redrawn and pushed only when
// its own parameters change.
//
// PLACEMENT IS FIXED AT COMPILE TIME (TTH_FACE_*_IN_PSRAM, Step 6.2 memory
// gate): both sprites in PSRAM. Each is allocated exactly there, its address
// is read back, and begin() fails -- the face stays blank and the log says why
// -- if it landed anywhere else. There is no fallback in either direction.
//
// BOTH EYES SHARE ONE SPRITE
//
// The two eyes must never show different blink phases. Making them one sprite,
// drawn from one FaceFrame via eyeRenderFor() and pushed in one operation,
// makes divergence impossible rather than merely unlikely.
//
// ATOMIC TRANSITIONS
//
// On a state change, every affected region is redrawn and then pushed inside a
// single startWrite()/endWrite() transaction in the same loop iteration. A
// transition is never left half-rendered across iterations. Continuous
// animation stays incremental: only the region that actually changed is
// redrawn and pushed.
class FaceRenderer {
 public:
  FaceRenderer();

  // Allocates each sprite in its configured region and paints the initial
  // face. Returns false if a sprite could not be allocated there.
  bool begin(const FaceGeometry& geometry);

  // Updates whatever changed this frame.
  //
  // `atomic` marks a state transition: everything dirty is drawn first, then
  // pushed together in one display transaction, so the whole face changes
  // within a single visual frame.
  void render(const FaceFrame& frame, bool atomic);

  void invalidateAll();

  // The activity menu, full screen: position ("2/4"), a title already reduced
  // to ASCII, and a bottom line (the touch-zone hint or a status).
  void drawActivityMenu(const char* position, const char* title, const char* bottom,
                        bool bottomIsHint);
  // Clears the menu and forces both face regions to be pushed again.
  void restoreAfterMenu();

  bool spritesAllocated() const { return _ready; }

  // Where the sprites ACTUALLY landed, read back from the canvas buffers
  // rather than from what was requested. Phase 5 decides whether they stay in
  // internal DRAM, so this must report the truth.
  const char* eyesRegion() const;
  const char* lowerFaceRegion() const;
  uint32_t spriteBytes() const;
  uint32_t pushCount() const { return _pushCount; }
  uint32_t maxPushMicros() const { return _maxPushMicros; }
  // Worst complete state-transition time: draw plus push for every region.
  uint32_t maxTransitionMicros() const { return _maxTransitionMicros; }
  void resetStats() {
    _pushCount = 0;
    _maxPushMicros = 0;
  }

 private:
  bool allocateSprite(M5Canvas& canvas, int width, int height,
                      const char* name, SpriteMemory memory);
  void drawEyes(const FaceFrame& frame);
  void drawLowerFace(const FaceFrame& frame);
  void drawFixedSmile(uint16_t color);
  bool eyeParamsChanged(const FaceFrame& frame) const;
  bool lowerParamsChanged(const FaceFrame& frame) const;
  void pushCanvas(M5Canvas& canvas, int x, int y);

  FaceGeometry _geometry;
  M5Canvas _eyes;
  M5Canvas _lowerFace;

  uint16_t _colorBackground;
  uint16_t _colorPrimary;
  uint16_t _colorDim;

  FaceFrame _renderedEyes;
  FaceFrame _renderedLower;

  bool _eyesDirty;
  bool _lowerDirty;
  bool _ready;

  uint32_t _pushCount;
  uint32_t _maxPushMicros;
  uint32_t _maxTransitionMicros;
};

}  // namespace tth
