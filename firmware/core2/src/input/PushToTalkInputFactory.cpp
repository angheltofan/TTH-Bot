#include "input/PushToTalkInputFactory.h"

#include "input/PhysicalPushToTalkInput.h"
#include "input/TouchPushToTalkInput.h"
#include "tth/Config.h"

namespace tth {

IPushToTalkInput& pushToTalkInput() {
#if TTH_PTT_INPUT == TTH_PTT_TOUCH
  static TouchPushToTalkInput input;
#elif TTH_PTT_INPUT == TTH_PTT_GPIO33
  static PhysicalPushToTalkInput input;
#else
#error "TTH_PTT_INPUT is not set to a known value"
#endif
  return input;
}

}  // namespace tth
