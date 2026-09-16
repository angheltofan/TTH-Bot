#include "tth/OutboundQueue.h"

#include <string.h>

namespace tth {

const char* toString(OutKind kind) {
  switch (kind) {
    case OutKind::TurnStart:
      return "turn_start";
    case OutKind::Audio:
      return "audio";
    case OutKind::TurnEnd:
      return "turn_end";
    case OutKind::Cancel:
      return "cancel";
    case OutKind::Credit:
      return "credit";
    case OutKind::Ping:
      return "ping";
  }
  return "invalid";
}

OutboundQueue::OutboundQueue()
    : _freeCount(kSlots),
      _orderedHead(0),
      _orderedCount(0),
      _priorityHead(0),
      _priorityCount(0),
      _inFlight(kNone),
      _inFlightPriority(false),
      _openTurn(0),
      _pendingCancelCount(0),
      _wireCount(0),
      _wireHead(0),
      _highWater(0) {
  for (uint32_t i = 0; i < kSlots; ++i) _free[i] = kSlots - 1 - i;
  for (uint32_t i = 0; i < kMaxPendingCancels; ++i) _pendingCancels[i] = 0;
  for (uint32_t i = 0; i < kWireMemory; ++i) _wire[i] = 0;
  memset(&_counters, 0, sizeof(_counters));
}

uint32_t OutboundQueue::takeSlot(bool mayUseReserve) {
  const uint32_t floor = mayUseReserve ? 0u : kReservedSlots;
  if (_freeCount <= floor) return kNone;
  const uint32_t slot = _free[--_freeCount];
  const uint32_t used = kSlots - _freeCount;
  if (used > _highWater) _highWater = used;
  return slot;
}

void OutboundQueue::releaseSlot(uint32_t slot) { _free[_freeCount++] = slot; }

void OutboundQueue::appendOrdered(uint32_t slot) {
  _ordered[(_orderedHead + _orderedCount) % kSlots] = slot;
  ++_orderedCount;
}

void OutboundQueue::appendPriority(uint32_t slot) {
  _priority[(_priorityHead + _priorityCount) % kSlots] = slot;
  ++_priorityCount;
}

bool OutboundQueue::reachedWire(uint32_t turn) const {
  for (uint32_t i = 0; i < _wireCount; ++i) {
    if (_wire[i] == turn) return true;
  }
  return false;
}

void OutboundQueue::rememberWire(uint32_t turn) {
  if (reachedWire(turn)) return;
  _wire[_wireHead] = turn;
  _wireHead = (_wireHead + 1) % kWireMemory;
  if (_wireCount < kWireMemory) ++_wireCount;
}

OutPush OutboundQueue::pushTurnStart(uint32_t turn) {
  if (turn == wire::kInvalidTurn || _openTurn != 0) return OutPush::Invalid;
  const uint32_t slot = takeSlot(true);
  if (slot == kNone) {
    ++_counters.busy;
    return OutPush::Busy;
  }
  OutboundItem& item = _items[slot];
  const size_t n = wire::encodeTurnStart(reinterpret_cast<char*>(item.bytes),
                                         sizeof(item.bytes), turn);
  item.kind = OutKind::TurnStart;
  item.lane = OutLane::Ordered;
  item.binary = false;
  item.turn = turn;
  item.length = static_cast<uint32_t>(n);
  appendOrdered(slot);
  _openTurn = turn;
  return OutPush::Accepted;
}

OutPush OutboundQueue::pushAudio(uint32_t turn, const int16_t* pcm,
                                 uint32_t samples) {
  if (turn == wire::kInvalidTurn || turn != _openTurn || pcm == nullptr ||
      samples == 0 || samples * 2u > wire::kMaxUpPcmBytes) {
    return OutPush::Invalid;
  }
  const uint32_t slot = takeSlot(false);
  if (slot == kNone) {
    ++_counters.busy;
    return OutPush::Busy;
  }
  OutboundItem& item = _items[slot];
  wire::writeAudioHeader(item.bytes, wire::kKindUserAudio, turn);
  uint8_t* out = item.bytes + wire::kAudioHeaderBytes;
  for (uint32_t i = 0; i < samples; ++i) {
    const uint16_t v = static_cast<uint16_t>(pcm[i]);
    out[2 * i] = static_cast<uint8_t>(v & 0xFFu);  // little-endian on every host
    out[2 * i + 1] = static_cast<uint8_t>(v >> 8);
  }
  item.kind = OutKind::Audio;
  item.lane = OutLane::Ordered;
  item.binary = true;
  item.turn = turn;
  item.length = static_cast<uint32_t>(wire::kAudioHeaderBytes) + samples * 2u;
  appendOrdered(slot);
  return OutPush::Accepted;
}

OutPush OutboundQueue::pushTurnEnd(uint32_t turn, uint32_t frames,
                                   uint32_t bytes) {
  if (turn == wire::kInvalidTurn || turn != _openTurn) return OutPush::Invalid;
  const uint32_t slot = takeSlot(true);
  if (slot == kNone) {
    ++_counters.busy;
    return OutPush::Busy;
  }
  OutboundItem& item = _items[slot];
  const size_t n = wire::encodeTurnEnd(reinterpret_cast<char*>(item.bytes),
                                       sizeof(item.bytes), turn, frames, bytes);
  item.kind = OutKind::TurnEnd;
  item.lane = OutLane::Ordered;
  item.binary = false;
  item.turn = turn;
  item.length = static_cast<uint32_t>(n);
  appendOrdered(slot);
  _openTurn = 0;
  return OutPush::Accepted;
}

void OutboundQueue::requestCancel(uint32_t turn) {
  if (turn == wire::kInvalidTurn) return;
  if (_openTurn == turn) _openTurn = 0;
  for (uint32_t i = 0; i < _pendingCancelCount; ++i) {
    if (_pendingCancels[i] == turn) return;
  }
  if (_pendingCancelCount < kMaxPendingCancels) {
    _pendingCancels[_pendingCancelCount++] = turn;
  } else {
    // Oldest pending cancel is superseded: shift and append.
    for (uint32_t i = 1; i < kMaxPendingCancels; ++i) {
      _pendingCancels[i - 1] = _pendingCancels[i];
    }
    _pendingCancels[kMaxPendingCancels - 1] = turn;
  }
}

OutPush OutboundQueue::pushCredit(uint32_t bytes) {
  if (bytes == 0) return OutPush::Accepted;
  // Coalesce into an unsent credit message rather than queue another.
  for (uint32_t i = 0; i < _priorityCount; ++i) {
    const uint32_t slot = _priority[(_priorityHead + i) % kSlots];
    if (slot == _inFlight) continue;
    OutboundItem& item = _items[slot];
    if (item.kind != OutKind::Credit) continue;
    const uint64_t sum = static_cast<uint64_t>(item.value) + bytes;
    if (sum > 0xFFFFFFFFull) break;  // would overflow: queue a second message
    item.value = static_cast<uint32_t>(sum);
    item.length = static_cast<uint32_t>(wire::encodeCredit(
        reinterpret_cast<char*>(item.bytes), sizeof(item.bytes), item.value));
    return OutPush::Accepted;
  }
  const uint32_t slot = takeSlot(true);
  if (slot == kNone) {
    ++_counters.busy;
    return OutPush::Busy;
  }
  OutboundItem& item = _items[slot];
  item.kind = OutKind::Credit;
  item.lane = OutLane::Priority;
  item.binary = false;
  item.turn = 0;
  item.value = bytes;
  item.length = static_cast<uint32_t>(wire::encodeCredit(
      reinterpret_cast<char*>(item.bytes), sizeof(item.bytes), bytes));
  appendPriority(slot);
  return OutPush::Accepted;
}

OutPush OutboundQueue::pushPing(uint32_t ts) {
  const uint32_t slot = takeSlot(false);
  if (slot == kNone) {
    ++_counters.busy;
    return OutPush::Busy;
  }
  OutboundItem& item = _items[slot];
  item.kind = OutKind::Ping;
  item.lane = OutLane::Priority;
  item.binary = false;
  item.turn = 0;
  item.length = static_cast<uint32_t>(wire::encodePing(
      reinterpret_cast<char*>(item.bytes), sizeof(item.bytes), ts));
  appendPriority(slot);
  return OutPush::Accepted;
}

void OutboundQueue::applyPendingCancels() {
  for (uint32_t c = 0; c < _pendingCancelCount; ++c) {
    const uint32_t turn = _pendingCancels[c];

    // 1. Purge every unsent ordered item of the turn. Nothing is in flight
    //    here: this only runs at a frame boundary.
    uint32_t kept = 0;
    uint32_t survivors[kSlots];
    for (uint32_t i = 0; i < _orderedCount; ++i) {
      const uint32_t slot = _ordered[(_orderedHead + i) % kSlots];
      if (_items[slot].turn == turn) {
        releaseSlot(slot);
        ++_counters.purged;
      } else {
        survivors[kept++] = slot;
      }
    }
    _orderedHead = 0;
    _orderedCount = kept;
    for (uint32_t i = 0; i < kept; ++i) _ordered[i] = survivors[i];

    // 2. Never on the wire: the gateway does not know this turn.
    if (!reachedWire(turn)) {
      ++_counters.cancelsDropped;
      continue;
    }

    // 3. Otherwise the cancel itself goes out on the priority lane, ahead of
    //    every remaining ordered item (i.e. before any later turn).
    const uint32_t slot = takeSlot(true);
    if (slot == kNone) {
      // Cannot happen after a purge of a live turn; counted if it ever does.
      ++_counters.busy;
      continue;
    }
    OutboundItem& item = _items[slot];
    item.kind = OutKind::Cancel;
    item.lane = OutLane::Priority;
    item.binary = false;
    item.turn = turn;
    item.length = static_cast<uint32_t>(wire::encodeCancel(
        reinterpret_cast<char*>(item.bytes), sizeof(item.bytes), turn));
    appendPriority(slot);
  }
  _pendingCancelCount = 0;
}

const OutboundItem* OutboundQueue::beginSend() {
  if (_inFlight != kNone) return &_items[_inFlight];  // still sending it
  applyPendingCancels();

  if (_priorityCount > 0) {
    _inFlight = _priority[_priorityHead];
    _inFlightPriority = true;
    return &_items[_inFlight];
  }
  if (_orderedCount > 0) {
    _inFlight = _ordered[_orderedHead];
    _inFlightPriority = false;
    return &_items[_inFlight];
  }
  return nullptr;
}

void OutboundQueue::completeSend() {
  if (_inFlight == kNone) return;
  const OutboundItem& item = _items[_inFlight];
  if (item.kind == OutKind::TurnStart) rememberWire(item.turn);
  if (item.kind == OutKind::Cancel) ++_counters.cancelsSent;
  ++_counters.sent;

  if (_inFlightPriority) {
    _priorityHead = (_priorityHead + 1) % kSlots;
    --_priorityCount;
  } else {
    _orderedHead = (_orderedHead + 1) % kSlots;
    --_orderedCount;
  }
  releaseSlot(_inFlight);
  _inFlight = kNone;
}

void OutboundQueue::failSend() {
  ++_counters.failedSends;
  // The connection is failing: nothing queued can be trusted to arrive in
  // order on a NEW connection, so everything goes, and the turn state with it.
  reset();
}

void OutboundQueue::reset() {
  _freeCount = kSlots;
  for (uint32_t i = 0; i < kSlots; ++i) _free[i] = kSlots - 1 - i;
  _orderedHead = 0;
  _orderedCount = 0;
  _priorityHead = 0;
  _priorityCount = 0;
  _inFlight = kNone;
  _openTurn = 0;
  _pendingCancelCount = 0;
  _wireCount = 0;
  _wireHead = 0;
}

}  // namespace tth
