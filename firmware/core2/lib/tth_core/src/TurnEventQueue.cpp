#include "tth/TurnEventQueue.h"

namespace tth {

const char* toString(PushResult result) {
  switch (result) {
    case PushResult::Accepted:
      return "accepted";
    case PushResult::Busy:
      return "busy";
    case PushResult::Fatal:
      return "fatal";
  }
  return "invalid";
}

const char* toString(TurnEventType type) {
  switch (type) {
    case TurnEventType::None:
      return "none";
    case TurnEventType::SpeechStart:
      return "speech start";
    case TurnEventType::TurnComplete:
      return "turn complete";
    case TurnEventType::Error:
      return "error";
  }
  return "invalid";
}

const char* toString(TurnError error) {
  switch (error) {
    case TurnError::None:
      return "none";
    case TurnError::Rejected:
      return "rejected";
    case TurnError::Injected:
      return "injected";
    case TurnError::BufferUnavailable:
      return "buffer unavailable";
    case TurnError::ResponseTimeout:
      return "no response in time";
    case TurnError::SessionLost:
      return "gateway session lost";
    case TurnError::GatewayError:
      return "gateway error";
    case TurnError::Protocol:
      return "protocol violation";
  }
  return "invalid";
}

TurnEventQueue::TurnEventQueue()
    : _head(0), _count(0), _drops(0), _staleDiscards(0) {
  for (uint32_t i = 0; i < kSlots; ++i) {
    _slots[i].type = TurnEventType::None;
    _slots[i].error = TurnError::None;
    _slots[i].format = monoS16(0);
    _slots[i].generation = 0;
  }
}

bool TurnEventQueue::push(const TurnEvent& event) {
  if (_count == kSlots) {
    ++_drops;
    return false;
  }
  _slots[(_head + _count) % kSlots] = event;
  ++_count;
  return true;
}

bool TurnEventQueue::pop(TurnEvent& out, uint32_t currentGeneration) {
  while (_count > 0) {
    const TurnEvent& front = _slots[_head];
    _head = (_head + 1) % kSlots;
    --_count;
    if (front.generation != currentGeneration) {
      ++_staleDiscards;
      continue;
    }
    out = front;
    return true;
  }
  return false;
}

void TurnEventQueue::clear() {
  _head = 0;
  _count = 0;
}

}  // namespace tth
