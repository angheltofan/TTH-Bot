#pragma once

#include "tth/IPushToTalkInput.h"

namespace tth {

// Returns the push-to-talk input selected by TTH_PTT_INPUT in
// include/tth/Config.h, which platformio.ini sets per environment:
//
//     core2-touch  -> TouchPushToTalkInput    (M5.BtnB)
//     core2-sw201  -> PhysicalPushToTalkInput (SW201 on GPIO 33)
//
// This function is the ONE place the choice is made. Nothing else in the
// firmware may name a concrete implementation -- that is what keeps the swap
// to the SW201 a configuration change rather than a code change.
//
// The instance is a function-local static: no heap, and its lifetime covers
// the whole program.
IPushToTalkInput& pushToTalkInput();

}  // namespace tth
