#include "tth/TurnIdAllocator.h"

namespace tth {

TurnIdAllocator::TurnIdAllocator(uint32_t first)
    : _next(first == 0 ? 1u : first),
      _active(0),
      _recentCount(0),
      _recentHead(0) {
  for (uint32_t i = 0; i < kRecent; ++i) _recent[i] = 0;
}

bool TurnIdAllocator::isRecent(uint32_t turn) const {
  if (turn == 0) return false;
  for (uint32_t i = 0; i < _recentCount; ++i) {
    if (_recent[i] == turn) return true;
  }
  return false;
}

bool TurnIdAllocator::collides(uint32_t candidate) const {
  return candidate == 0 || candidate == _active || isRecent(candidate);
}

void TurnIdAllocator::remember(uint32_t turn) {
  if (turn == 0 || isRecent(turn)) return;
  _recent[_recentHead] = turn;
  _recentHead = (_recentHead + 1) % kRecent;
  if (_recentCount < kRecent) ++_recentCount;
}

void TurnIdAllocator::finish(uint32_t turn) {
  remember(turn);
  if (turn == _active) _active = 0;
}

uint32_t TurnIdAllocator::allocate() {
  if (_active != 0) finish(_active);
  // At most kRecent + 2 candidates can collide, so this always terminates.
  for (;;) {
    const uint32_t candidate = _next;
    _next = (candidate == 0xFFFFFFFFu) ? 1u : candidate + 1u;
    if (!collides(candidate)) {
      _active = candidate;
      return candidate;
    }
  }
}

}  // namespace tth
