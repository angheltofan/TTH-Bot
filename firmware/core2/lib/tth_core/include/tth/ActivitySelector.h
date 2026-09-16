#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tth/ConfigStore.h"
#include "tth/GatewayProtocol.h"

// On-device activity selection (touch zones: left opens / previous, right
// next, centre confirms).
//
// The device only ever holds activity METADATA: id, a bounded title and the
// mode. Prompts and participant data never reach it. Everything here is
// fixed-size and allocation-free.
//
//   ActivityCatalog        the gateway's list, received one item per
//                          activity_list message; the previous complete list
//                          stays in use until a new one is complete
//   ActivitySelector       the menu state machine
//   activityMenuRefusal()  when the menu may open at all
//   ActivityPreference     the saved selection (the id only) in NVS
//   toDisplayAscii()       a title for the built-in ASCII font
//
// Portable.

namespace tth {

struct ActivityEntry {
  char id[wire::kActivityIdChars + 1];
  char title[wire::kMaxActivityTitleBytes + 1];
  bool pushToTalk;
};

class ActivityCatalog {
 public:
  static const uint32_t kMax = wire::kMaxActivities;

  enum class Accept : uint8_t { Partial = 0, Complete, Rejected };

  ActivityCatalog();

  // A new connection: no list, no current activity.
  void reset();

  // One activity_list item (already validated by the parser). Items must
  // arrive in order 0..count-1 with the same count; anything else discards
  // the partial list (the last complete one is kept).
  Accept accept(const wire::ControlMessage& item);

  // The activity the gateway is using (ready / activity_selected).
  void setCurrent(const char* id);

  bool complete() const { return _count > 0; }
  uint32_t count() const { return _count; }
  const ActivityEntry& at(uint32_t index) const;
  int32_t indexOf(const char* id) const;
  const char* current() const { return _current; }
  uint32_t listErrors() const { return _listErrors; }

 private:
  ActivityEntry _entries[kMax];
  uint32_t _count;
  ActivityEntry _staging[kMax];
  uint32_t _stagingCount;
  uint32_t _stagingReceived;
  char _stagingCurrent[wire::kActivityIdChars + 1];
  char _current[wire::kActivityIdChars + 1];
  uint32_t _listErrors;
};

enum class SelectorState : uint8_t {
  Closed = 0,
  Browsing,  // menu shown; left/right move, centre confirms
  Pending,   // activity_select sent; waiting for the gateway
  Failed,    // showing a failure briefly; then closes
};

enum class SelectorEvent : uint8_t {
  None = 0,
  Opened,
  Moved,
  Unchanged,        // confirmed the activity already in use: closed, no request
  SelectRequested,  // send activity_select(pendingId())
  Selected,         // the gateway confirmed: closed
  Failed,           // refused or timed out: previous activity stays
  TimedOut,         // no input for the idle timeout: closed
  Dismissed,        // the failure was shown: closed
  Cancelled,        // closed by the application (session lost, busy)
};

const char* toString(SelectorState state);
const char* toString(SelectorEvent event);

class ActivitySelector {
 public:
  ActivitySelector(uint32_t idleTimeoutMs, uint32_t pendingTimeoutMs,
                   uint32_t failDisplayMs);

  // Opens on the current activity (or the first). None when already open or
  // the catalog is empty. The caller checks activityMenuRefusal() first.
  SelectorEvent open(const ActivityCatalog& catalog, uint32_t nowMs);
  // Wrap-around navigation. Only while Browsing.
  SelectorEvent previous(const ActivityCatalog& catalog, uint32_t nowMs);
  SelectorEvent next(const ActivityCatalog& catalog, uint32_t nowMs);
  // Only while Browsing.
  SelectorEvent confirm(const ActivityCatalog& catalog, uint32_t nowMs);

  // Replies from the gateway.
  SelectorEvent onSelected(const char* id, uint32_t nowMs);
  SelectorEvent onError(uint32_t nowMs);

  SelectorEvent cancel();
  SelectorEvent tick(uint32_t nowMs);

  SelectorState state() const { return _state; }
  // While open, the centre zone belongs to the menu: push-to-talk is swallowed.
  bool isOpen() const { return _state != SelectorState::Closed; }
  uint32_t highlight() const { return _highlight; }
  const char* pendingId() const { return _pendingId; }
  uint32_t selections() const { return _selections; }
  uint32_t failures() const { return _failures; }
  uint32_t timeouts() const { return _timeouts; }

 private:
  const uint32_t _idleTimeoutMs;
  const uint32_t _pendingTimeoutMs;
  const uint32_t _failDisplayMs;
  SelectorState _state;
  uint32_t _highlight;
  uint32_t _sinceMs;
  char _pendingId[wire::kActivityIdChars + 1];
  uint32_t _selections;
  uint32_t _failures;
  uint32_t _timeouts;
};

struct ActivityMenuConditions {
  bool online;             // Wi-Fi and the gateway session are up
  bool sessionReady;       // GatewaySession::Ready
  bool conversationReady;  // ConversationState::Ready
  bool audioIdle;          // no capture, no playback
  bool turnIdle;           // no streamed or awaited turn
  bool bargeInIdle;
  bool catalogReady;       // a complete, non-empty list
  bool usingGateway;       // not a diagnostic local mock
};

// nullptr when the menu may open; otherwise a short fixed reason for the log.
const char* activityMenuRefusal(const ActivityMenuConditions& c);

// The saved selection: the activity id only, never a prompt.
class ActivityPreference {
 public:
  static const char* const kKey;

  explicit ActivityPreference(IConfigStorage& storage);

  // Copies the saved id into `out` (>= 37 bytes). False, with out = "", when
  // none is saved or the stored value is not a valid id.
  bool load(char* out, size_t capacity);
  bool save(const char* id);
  bool clear();

 private:
  IConfigStorage& _storage;
};

// UTF-8 title -> printable ASCII for the built-in font: Romanian letters lose
// their diacritics (ă â -> a, î -> i, ș ş -> s, ț ţ -> t, and capitals), any
// other non-ASCII character becomes '?', control characters are dropped.
// Always NUL-terminated; returns the output length.
size_t toDisplayAscii(const char* utf8, char* out, size_t capacity);

}  // namespace tth
