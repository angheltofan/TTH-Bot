#include "input/TouchPushToTalkInput.h"

#include <M5Unified.h>

#include "tth/Config.h"

namespace tth {

// Debounce 0: the FT6336U touch controller is already debounced inside
// M5Unified, so a second debounce here would only add latency to a press.
// The maximum-hold ceiling still applies, identically to the SW201.
TouchPushToTalkInput::TouchPushToTalkInput()
    : _button(0, TTH_PTT_MAX_HOLD_MS) {}

void TouchPushToTalkInput::begin() {
  // Nothing to configure: M5.begin() already brought up the touch controller,
  // and M5.update() latches the button state each loop.
}

void TouchPushToTalkInput::poll(uint32_t nowMs) {
  _button.update(M5.BtnB.isPressed(), nowMs);
}

const char* TouchPushToTalkInput::name() const {
  return "M5.BtnB (centre capacitive touch zone)";
}

}  // namespace tth
