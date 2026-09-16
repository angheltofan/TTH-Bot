#include "input/PhysicalPushToTalkInput.h"

#include <Arduino.h>

#include "tth/Config.h"

namespace tth {

// A mechanical switch bounces; TTH_PTT_DEBOUNCE_MS is the settling window.
PhysicalPushToTalkInput::PhysicalPushToTalkInput()
    : _button(TTH_PTT_DEBOUNCE_MS, TTH_PTT_MAX_HOLD_MS) {}

void PhysicalPushToTalkInput::begin() {
  pinMode(TTH_PTT_GPIO_PIN, INPUT_PULLUP);
}

void PhysicalPushToTalkInput::poll(uint32_t nowMs) {
  // Active low: LOW means pressed.
  _button.update(digitalRead(TTH_PTT_GPIO_PIN) == LOW, nowMs);
}

const char* PhysicalPushToTalkInput::name() const {
  return "SW201 on GPIO 33 (normally open to GND, internal pull-up)";
}

}  // namespace tth
