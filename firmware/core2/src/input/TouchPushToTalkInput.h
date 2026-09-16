#pragma once

#include "tth/IPushToTalkInput.h"
#include "tth/PttButton.h"

namespace tth {

// Hold-to-talk on M5.BtnB -- the CENTRE of the three capacitive touch zones in
// the strip below the display. That strip is physically outside the 320x240
// display area, so using it leaves the whole screen free for the robot face.
//
// The class is a thin adapter: all timing behaviour lives in PttButton, shared
// with PhysicalPushToTalkInput.
class TouchPushToTalkInput : public IPushToTalkInput {
 public:
  TouchPushToTalkInput();

  void begin() override;
  void poll(uint32_t nowMs) override;
  bool isHeld() const override { return _button.isHeld(); }
  bool consumePress() override { return _button.consumePress(); }
  bool consumeRelease() override { return _button.consumeRelease(); }
  uint32_t heldForMs(uint32_t nowMs) const override {
    return _button.heldForMs(nowMs);
  }
  uint32_t lastHoldMs() const override { return _button.lastHoldMs(); }
  bool lastReleaseWasForced() const override {
    return _button.lastReleaseWasForced();
  }
  const char* name() const override;

 private:
  PttButton _button;
};

}  // namespace tth
