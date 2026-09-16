#include "tth/ActivitySelector.h"

#include <string.h>

namespace tth {

namespace {

void copyId(char* dest, const char* src) {
  if (src == nullptr || !wire::isActivityId(src)) {
    dest[0] = '\0';
    return;
  }
  memcpy(dest, src, wire::kActivityIdChars);
  dest[wire::kActivityIdChars] = '\0';
}

}  // namespace

// --- ActivityCatalog ---------------------------------------------------------------

ActivityCatalog::ActivityCatalog() : _listErrors(0) { reset(); }

void ActivityCatalog::reset() {
  memset(_entries, 0, sizeof(_entries));
  memset(_staging, 0, sizeof(_staging));
  _count = 0;
  _stagingCount = 0;
  _stagingReceived = 0;
  _stagingCurrent[0] = '\0';
  _current[0] = '\0';
}

ActivityCatalog::Accept ActivityCatalog::accept(const wire::ControlMessage& item) {
  if (item.type != wire::ControlType::ActivityList || item.count == 0 || item.count > kMax ||
      item.index >= item.count || !wire::isActivityId(item.activity)) {
    ++_listErrors;
    return Accept::Rejected;
  }
  if (item.index == 0) {
    _stagingCount = item.count;
    _stagingReceived = 0;
    _stagingCurrent[0] = '\0';
  } else if (item.count != _stagingCount || item.index != _stagingReceived) {
    // Out of order or a different list: drop the partial one.
    _stagingCount = 0;
    _stagingReceived = 0;
    ++_listErrors;
    return Accept::Rejected;
  }
  ActivityEntry& entry = _staging[item.index];
  copyId(entry.id, item.activity);
  const size_t titleLength = strnlen(item.title, wire::kMaxActivityTitleBytes);
  memcpy(entry.title, item.title, titleLength);
  entry.title[titleLength] = '\0';
  entry.pushToTalk = strcmp(item.mode, "push_to_talk") == 0;
  if (item.current) copyId(_stagingCurrent, item.activity);
  ++_stagingReceived;

  if (_stagingReceived < _stagingCount) return Accept::Partial;

  memcpy(_entries, _staging, sizeof(_entries));
  _count = _stagingCount;
  if (_stagingCurrent[0] != '\0') memcpy(_current, _stagingCurrent, sizeof(_current));
  _stagingCount = 0;
  _stagingReceived = 0;
  return Accept::Complete;
}

void ActivityCatalog::setCurrent(const char* id) { copyId(_current, id); }

const ActivityEntry& ActivityCatalog::at(uint32_t index) const {
  return _entries[index < kMax ? index : 0];
}

int32_t ActivityCatalog::indexOf(const char* id) const {
  if (id == nullptr || id[0] == '\0') return -1;
  for (uint32_t i = 0; i < _count; ++i) {
    if (strcmp(_entries[i].id, id) == 0) return static_cast<int32_t>(i);
  }
  return -1;
}

// --- ActivitySelector -----------------------------------------------------------------

const char* toString(SelectorState state) {
  switch (state) {
    case SelectorState::Closed:
      return "closed";
    case SelectorState::Browsing:
      return "browsing";
    case SelectorState::Pending:
      return "pending";
    case SelectorState::Failed:
      return "failed";
  }
  return "invalid";
}

const char* toString(SelectorEvent event) {
  switch (event) {
    case SelectorEvent::None:
      return "none";
    case SelectorEvent::Opened:
      return "opened";
    case SelectorEvent::Moved:
      return "moved";
    case SelectorEvent::Unchanged:
      return "unchanged";
    case SelectorEvent::SelectRequested:
      return "select requested";
    case SelectorEvent::Selected:
      return "selected";
    case SelectorEvent::Failed:
      return "failed";
    case SelectorEvent::TimedOut:
      return "timed out";
    case SelectorEvent::Dismissed:
      return "dismissed";
    case SelectorEvent::Cancelled:
      return "cancelled";
  }
  return "invalid";
}

ActivitySelector::ActivitySelector(uint32_t idleTimeoutMs, uint32_t pendingTimeoutMs,
                                   uint32_t failDisplayMs)
    : _idleTimeoutMs(idleTimeoutMs),
      _pendingTimeoutMs(pendingTimeoutMs),
      _failDisplayMs(failDisplayMs),
      _state(SelectorState::Closed),
      _highlight(0),
      _sinceMs(0),
      _selections(0),
      _failures(0),
      _timeouts(0) {
  _pendingId[0] = '\0';
}

SelectorEvent ActivitySelector::open(const ActivityCatalog& catalog, uint32_t nowMs) {
  if (_state != SelectorState::Closed || catalog.count() == 0) return SelectorEvent::None;
  const int32_t current = catalog.indexOf(catalog.current());
  _highlight = current >= 0 ? static_cast<uint32_t>(current) : 0;
  _state = SelectorState::Browsing;
  _sinceMs = nowMs;
  _pendingId[0] = '\0';
  return SelectorEvent::Opened;
}

SelectorEvent ActivitySelector::previous(const ActivityCatalog& catalog, uint32_t nowMs) {
  if (_state != SelectorState::Browsing || catalog.count() == 0) return SelectorEvent::None;
  _highlight = (_highlight == 0 || _highlight >= catalog.count()) ? catalog.count() - 1
                                                                  : _highlight - 1;
  _sinceMs = nowMs;
  return SelectorEvent::Moved;
}

SelectorEvent ActivitySelector::next(const ActivityCatalog& catalog, uint32_t nowMs) {
  if (_state != SelectorState::Browsing || catalog.count() == 0) return SelectorEvent::None;
  _highlight = (_highlight + 1 >= catalog.count()) ? 0 : _highlight + 1;
  _sinceMs = nowMs;
  return SelectorEvent::Moved;
}

SelectorEvent ActivitySelector::confirm(const ActivityCatalog& catalog, uint32_t nowMs) {
  if (_state != SelectorState::Browsing) return SelectorEvent::None;
  if (_highlight >= catalog.count()) {
    _state = SelectorState::Closed;
    return SelectorEvent::Cancelled;
  }
  const ActivityEntry& entry = catalog.at(_highlight);
  if (strcmp(entry.id, catalog.current()) == 0) {
    _state = SelectorState::Closed;
    return SelectorEvent::Unchanged;
  }
  copyId(_pendingId, entry.id);
  _state = SelectorState::Pending;
  _sinceMs = nowMs;
  return SelectorEvent::SelectRequested;
}

SelectorEvent ActivitySelector::onSelected(const char* id, uint32_t nowMs) {
  (void)nowMs;
  if (_state != SelectorState::Pending || id == nullptr || strcmp(id, _pendingId) != 0) {
    return SelectorEvent::None;
  }
  _state = SelectorState::Closed;
  ++_selections;
  return SelectorEvent::Selected;
}

SelectorEvent ActivitySelector::onError(uint32_t nowMs) {
  if (_state != SelectorState::Pending) return SelectorEvent::None;
  _state = SelectorState::Failed;
  _sinceMs = nowMs;
  ++_failures;
  return SelectorEvent::Failed;
}

SelectorEvent ActivitySelector::cancel() {
  if (_state == SelectorState::Closed) return SelectorEvent::None;
  _state = SelectorState::Closed;
  _pendingId[0] = '\0';
  return SelectorEvent::Cancelled;
}

SelectorEvent ActivitySelector::tick(uint32_t nowMs) {
  const uint32_t elapsed = nowMs - _sinceMs;
  switch (_state) {
    case SelectorState::Browsing:
      if (elapsed >= _idleTimeoutMs) {
        _state = SelectorState::Closed;
        ++_timeouts;
        return SelectorEvent::TimedOut;
      }
      return SelectorEvent::None;
    case SelectorState::Pending:
      if (elapsed >= _pendingTimeoutMs) {
        _state = SelectorState::Failed;
        _sinceMs = nowMs;
        ++_failures;
        return SelectorEvent::Failed;
      }
      return SelectorEvent::None;
    case SelectorState::Failed:
      if (elapsed >= _failDisplayMs) {
        _state = SelectorState::Closed;
        _pendingId[0] = '\0';
        return SelectorEvent::Dismissed;
      }
      return SelectorEvent::None;
    case SelectorState::Closed:
      return SelectorEvent::None;
  }
  return SelectorEvent::None;
}

// --- gate ---------------------------------------------------------------------------

const char* activityMenuRefusal(const ActivityMenuConditions& c) {
  if (!c.online || !c.sessionReady) return "not online";
  if (!c.usingGateway) return "local mock in use";
  if (!c.conversationReady) return "not idle";
  if (!c.audioIdle) return "audio busy";
  if (!c.turnIdle) return "turn in progress";
  if (!c.bargeInIdle) return "barge-in in progress";
  if (!c.catalogReady) return "no activity list yet";
  return nullptr;
}

// --- ActivityPreference ---------------------------------------------------------------

const char* const ActivityPreference::kKey = "act";

ActivityPreference::ActivityPreference(IConfigStorage& storage) : _storage(storage) {}

bool ActivityPreference::load(char* out, size_t capacity) {
  if (out == nullptr || capacity < wire::kActivityIdChars + 1) return false;
  out[0] = '\0';
  uint8_t buffer[wire::kActivityIdChars];
  if (_storage.read(kKey, buffer, sizeof(buffer)) != sizeof(buffer)) return false;
  char text[wire::kActivityIdChars + 1];
  memcpy(text, buffer, wire::kActivityIdChars);
  text[wire::kActivityIdChars] = '\0';
  if (!wire::isActivityId(text)) return false;
  memcpy(out, text, sizeof(text));
  return true;
}

bool ActivityPreference::save(const char* id) {
  if (!wire::isActivityId(id)) return false;
  return _storage.write(kKey, reinterpret_cast<const uint8_t*>(id), wire::kActivityIdChars);
}

bool ActivityPreference::clear() {
  const uint8_t zeros[wire::kActivityIdChars] = {0};
  return _storage.write(kKey, zeros, sizeof(zeros));
}

// --- display text --------------------------------------------------------------------

size_t toDisplayAscii(const char* utf8, char* out, size_t capacity) {
  if (out == nullptr || capacity == 0) return 0;
  size_t n = 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8 != nullptr ? utf8 : "");
  while (*p != 0 && n + 1 < capacity) {
    const unsigned char b = *p;
    if (b < 0x80) {
      ++p;
      if (b >= 0x20 && b != 0x7f) out[n++] = static_cast<char>(b);
      continue;
    }
    // Decode one multi-byte sequence (invalid ones become a single '?').
    uint32_t cp = 0;
    size_t extra = 0;
    if ((b & 0xE0) == 0xC0) {
      cp = b & 0x1F;
      extra = 1;
    } else if ((b & 0xF0) == 0xE0) {
      cp = b & 0x0F;
      extra = 2;
    } else if ((b & 0xF8) == 0xF0) {
      cp = b & 0x07;
      extra = 3;
    } else {
      ++p;
      out[n++] = '?';
      continue;
    }
    ++p;
    bool valid = true;
    for (size_t i = 0; i < extra; ++i) {
      if ((*p & 0xC0) != 0x80) {
        valid = false;
        break;
      }
      cp = (cp << 6) | (*p & 0x3F);
      ++p;
    }
    char mapped = '?';
    if (valid) {
      switch (cp) {
        case 0x0103:  // ă
        case 0x00E2:  // â
          mapped = 'a';
          break;
        case 0x0102:  // Ă
        case 0x00C2:  // Â
          mapped = 'A';
          break;
        case 0x00EE:  // î
          mapped = 'i';
          break;
        case 0x00CE:  // Î
          mapped = 'I';
          break;
        case 0x0219:  // ș
        case 0x015F:  // ş
          mapped = 's';
          break;
        case 0x0218:  // Ș
        case 0x015E:  // Ş
          mapped = 'S';
          break;
        case 0x021B:  // ț
        case 0x0163:  // ţ
          mapped = 't';
          break;
        case 0x021A:  // Ț
        case 0x0162:  // Ţ
          mapped = 'T';
          break;
        default:
          break;
      }
    }
    out[n++] = mapped;
  }
  out[n] = '\0';
  return n;
}

}  // namespace tth
