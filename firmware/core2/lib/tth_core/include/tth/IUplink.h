#pragma once

#include <stdint.h>

#include "tth/OutboundQueue.h"

// What a gateway turn source may do with the ONE ordered outbound path
// (docs/PHASE6_PLAN.md §3.1-3.2, Step 6.3).
//
// On the device this is the OutboundQueue shared with the NetSender task under
// a mutex (GatewayClient); in the host tests it is the same OutboundQueue with
// a test sender. Every call is made by the loop and returns at once.
//
// DELIVERY NOTIFICATION
//
// lastTurnEndSent() is published by the SENDER after completeSend() of a
// turn_end: the whole frame was written to the socket. It is not "queued" --
// that is what starts the first-response timer. Turn ids never repeat within
// the recent window (TurnIdAllocator), so equality with the active turn is
// meaningful and a published value can never be lost the way an event can.

namespace tth {

class IUplink {
 public:
  virtual ~IUplink() {}
  virtual OutPush pushTurnStart(uint32_t turn) = 0;
  virtual OutPush pushAudio(uint32_t turn, const int16_t* pcm, uint32_t samples) = 0;
  virtual OutPush pushTurnEnd(uint32_t turn, uint32_t frames, uint32_t bytes) = 0;
  // Applied by the sender at the next frame boundary; never overtakes a frame
  // that is partially sent.
  virtual void requestCancel(uint32_t turn) = 0;
  virtual OutPush pushCredit(uint32_t bytes) = 0;
  virtual uint32_t lastTurnEndSent() const = 0;
};

}  // namespace tth
