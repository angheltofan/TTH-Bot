#pragma once

#include "tth/IPushToTalkInput.h"
#include "tth/PttButton.h"

namespace tth {

// Hold-to-talk on a normally-open SW201 momentary push button wired between
// GPIO 33 and GND.
//
// Active low: INPUT_PULLUP idles the pin high and pressing the button pulls it
// to ground, so no external resistor is needed. GPIO 33 is Port A's SCL line,
// which means the SW201 and any Port A I2C unit are mutually exclusive.
//
// Compiled in EVERY build, including core2-touch, so this path cannot rot
// while the button is unavailable.
class PhysicalPushToTalkInput : public IPushToTalkInput {
 public:
  PhysicalPushToTalkInput();

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
