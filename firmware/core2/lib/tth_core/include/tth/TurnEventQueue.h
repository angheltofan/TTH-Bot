#pragma once

#include <stdint.h>

#include "tth/ITurnSource.h"

// A bounded FIFO of turn-source control events.
//
// Holds small value types only -- never audio, never pointers. A turn produces
// at most three events (SpeechStart, TurnComplete or Error) and the loop drains
// the queue every iteration, so eight slots is generous.
//
// FULL: the NEWEST event is refused and counted. Lifecycle events are never
// silently reordered or replaced, and a non-zero drop count is a visible
// defect in the heartbeat rather than a quiet one.
//
// STALE: events carry the generation they were produced in. pop() discards any
// event from an older generation -- that is how "no late delivery after
// cancel()" is enforced, not merely intended.

namespace tth {

class TurnEventQueue {
 public:
  static const uint32_t kSlots = 8;

  TurnEventQueue();

  bool push(const TurnEvent& event);

  // The oldest event stamped with `currentGeneration`. Older-generation events
  // in front of it are discarded and counted.
  bool pop(TurnEvent& out, uint32_t currentGeneration);

  void clear();
  uint32_t size() const { return _count; }
  uint32_t drops() const { return _drops; }
  uint32_t staleDiscards() const { return _staleDiscards; }

 private:
  TurnEvent _slots[kSlots];
  uint32_t _head;
  uint32_t _count;
  uint32_t _drops;
  uint32_t _staleDiscards;
};

}  // namespace tth
