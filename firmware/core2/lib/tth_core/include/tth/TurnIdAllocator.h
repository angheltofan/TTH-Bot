#pragma once

#include <stdint.h>

// uint32 turn ids for the gateway protocol (docs/PHASE6_PLAN.md §2.2).
//
// Ids start at 1 and increment; 0xFFFFFFFF wraps to 1, because 0 is reserved
// and never used. A new id never equals the active turn or any of the last
// kRecent ended or cancelled turns -- a late frame for an old turn can then
// never be mistaken for the new one.
//
// Portable; loop task only.

namespace tth {

class TurnIdAllocator {
 public:
  static const uint32_t kRecent = 16;

  // `first` is where allocation starts (1 normally; tests start near the
  // wrap). 0 is treated as 1.
  explicit TurnIdAllocator(uint32_t first = 1);

  // A fresh id, which also becomes the active turn. A still-active previous
  // turn is finished first.
  uint32_t allocate();

  // The turn ended or was cancelled: it joins the recent set.
  void finish(uint32_t turn);

  uint32_t active() const { return _active; }
  bool isRecent(uint32_t turn) const;

 private:
  bool collides(uint32_t candidate) const;
  void remember(uint32_t turn);

  uint32_t _next;
  uint32_t _active;
  uint32_t _recent[kRecent];
  uint32_t _recentCount;
  uint32_t _recentHead;
};

}  // namespace tth
