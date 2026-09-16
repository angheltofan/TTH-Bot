// Host-side tests for on-device activity selection: the catalog, the menu
// state machine, the gate that keeps the menu away from a busy robot, the
// saved selection, and the display text.

#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include <unity.h>

#include "tth/ActivitySelector.h"
#include "tth/GatewayProtocol.h"

using tth::ActivityCatalog;
using tth::ActivityMenuConditions;
using tth::ActivityPreference;
using tth::ActivitySelector;
using tth::SelectorEvent;
using tth::SelectorState;
using tth::wire::ControlError;
using tth::wire::ControlMessage;

void setUp() {}
void tearDown() {}

namespace {

const char* const kIds[] = {
    "00000000-0000-4000-8000-000000000001",
    "00000000-0000-4000-8000-000000000002",
    "00000000-0000-4000-8000-000000000003",
    "00000000-0000-4000-8000-000000000004",
};

ControlMessage item(uint32_t index, uint32_t count, const char* id, bool current,
                    const char* title = "Activitate") {
  char text[513];
  snprintf(text, sizeof(text),
           "{\"t\":\"activity_list\",\"index\":%u,\"count\":%u,\"activity\":\"%s\","
           "\"title\":\"%s\",\"mode\":\"push_to_talk\",\"current\":%s}",
           static_cast<unsigned>(index), static_cast<unsigned>(count), id, title,
           current ? "true" : "false");
  ControlMessage m;
  TEST_ASSERT_TRUE(tth::wire::parseControl(text, strlen(text), m) == ControlError::None);
  return m;
}

// A complete list of `count` items, current = `currentIndex`.
void load(ActivityCatalog& catalog, uint32_t count, uint32_t currentIndex) {
  for (uint32_t i = 0; i < count; ++i) {
    const ActivityCatalog::Accept a = catalog.accept(item(i, count, kIds[i], i == currentIndex));
    TEST_ASSERT_TRUE(a == (i + 1 == count ? ActivityCatalog::Accept::Complete
                                          : ActivityCatalog::Accept::Partial));
  }
}

class MemoryStorage : public tth::IConfigStorage {
 public:
  std::map<std::string, std::vector<uint8_t>> data;
  size_t read(const char* key, uint8_t* out, size_t capacity) override {
    auto it = data.find(key);
    if (it == data.end()) return 0;
    if (it->second.size() <= capacity) memcpy(out, it->second.data(), it->second.size());
    return it->second.size();
  }
  bool write(const char* key, const uint8_t* bytes, size_t length) override {
    data[key].assign(bytes, bytes + length);
    return true;
  }
  bool eraseAll() override {
    data.clear();
    return true;
  }
};

ActivityMenuConditions idle() {
  ActivityMenuConditions c;
  c.online = true;
  c.sessionReady = true;
  c.conversationReady = true;
  c.audioIdle = true;
  c.turnIdle = true;
  c.bargeInIdle = true;
  c.catalogReady = true;
  c.usingGateway = true;
  return c;
}

}  // namespace

// --- catalog --------------------------------------------------------------------------

static void a_list_is_used_only_once_complete() {
  ActivityCatalog catalog;
  TEST_ASSERT_TRUE(catalog.accept(item(0, 3, kIds[0], false)) == ActivityCatalog::Accept::Partial);
  TEST_ASSERT_EQUAL_UINT32(0, catalog.count());
  TEST_ASSERT_TRUE(catalog.accept(item(1, 3, kIds[1], true)) == ActivityCatalog::Accept::Partial);
  TEST_ASSERT_TRUE(catalog.accept(item(2, 3, kIds[2], false)) == ActivityCatalog::Accept::Complete);
  TEST_ASSERT_EQUAL_UINT32(3, catalog.count());
  TEST_ASSERT_EQUAL_STRING(kIds[1], catalog.current());
  TEST_ASSERT_EQUAL_INT32(2, catalog.indexOf(kIds[2]));
  TEST_ASSERT_EQUAL_INT32(-1, catalog.indexOf("00000000-0000-4000-8000-00000000000f"));
  TEST_ASSERT_TRUE(catalog.at(0).pushToTalk);
}

static void an_out_of_order_list_is_discarded_and_the_old_one_kept() {
  ActivityCatalog catalog;
  load(catalog, 2, 0);
  TEST_ASSERT_TRUE(catalog.accept(item(0, 3, kIds[2], false)) == ActivityCatalog::Accept::Partial);
  TEST_ASSERT_TRUE(catalog.accept(item(2, 3, kIds[3], false)) == ActivityCatalog::Accept::Rejected);
  TEST_ASSERT_EQUAL_UINT32(2, catalog.count());
  TEST_ASSERT_EQUAL_STRING(kIds[0], catalog.at(0).id);
  TEST_ASSERT_TRUE(catalog.accept(item(1, 4, kIds[1], false)) == ActivityCatalog::Accept::Rejected);
  TEST_ASSERT_EQUAL_UINT32(2, catalog.listErrors());
  catalog.reset();
  TEST_ASSERT_EQUAL_UINT32(0, catalog.count());
  TEST_ASSERT_EQUAL_STRING("", catalog.current());
}

static void a_title_is_stored_bounded_and_never_a_prompt() {
  ActivityCatalog catalog;
  const std::string title(48, 'T');
  TEST_ASSERT_TRUE(catalog.accept(item(0, 1, kIds[0], true, title.c_str())) ==
                   ActivityCatalog::Accept::Complete);
  TEST_ASSERT_EQUAL_UINT32(48, strlen(catalog.at(0).title));
  // The only fields an entry has are id, title and mode.
  TEST_ASSERT_EQUAL_UINT32(sizeof(tth::ActivityEntry),
                           sizeof(catalog.at(0).id) + sizeof(catalog.at(0).title) + sizeof(bool));
}

// --- selector ---------------------------------------------------------------------------

static void the_menu_opens_on_the_current_activity_and_wraps() {
  ActivityCatalog catalog;
  load(catalog, 4, 2);
  ActivitySelector s(10000, 30000, 2000);
  TEST_ASSERT_TRUE(s.open(catalog, 0) == SelectorEvent::Opened);
  TEST_ASSERT_TRUE(s.isOpen());
  TEST_ASSERT_EQUAL_UINT32(2, s.highlight());
  TEST_ASSERT_TRUE(s.open(catalog, 0) == SelectorEvent::None);  // already open
  TEST_ASSERT_TRUE(s.next(catalog, 100) == SelectorEvent::Moved);
  TEST_ASSERT_EQUAL_UINT32(3, s.highlight());
  s.next(catalog, 200);
  TEST_ASSERT_EQUAL_UINT32(0, s.highlight());  // wrap forward
  s.previous(catalog, 300);
  TEST_ASSERT_EQUAL_UINT32(3, s.highlight());  // wrap back
  s.previous(catalog, 400);
  TEST_ASSERT_EQUAL_UINT32(2, s.highlight());
}

static void an_empty_catalog_never_opens() {
  ActivityCatalog catalog;
  ActivitySelector s(10000, 30000, 2000);
  TEST_ASSERT_TRUE(s.open(catalog, 0) == SelectorEvent::None);
  TEST_ASSERT_FALSE(s.isOpen());
}

static void confirming_another_activity_requests_it_and_waits() {
  ActivityCatalog catalog;
  load(catalog, 4, 0);
  ActivitySelector s(10000, 30000, 2000);
  s.open(catalog, 0);
  s.next(catalog, 10);
  TEST_ASSERT_TRUE(s.confirm(catalog, 20) == SelectorEvent::SelectRequested);
  TEST_ASSERT_TRUE(s.state() == SelectorState::Pending);
  TEST_ASSERT_EQUAL_STRING(kIds[1], s.pendingId());
  // Still open: the centre zone stays away from push-to-talk until confirmed.
  TEST_ASSERT_TRUE(s.isOpen());
  TEST_ASSERT_TRUE(s.next(catalog, 30) == SelectorEvent::None);
  TEST_ASSERT_TRUE(s.confirm(catalog, 30) == SelectorEvent::None);
  // A reply for another id is not ours.
  TEST_ASSERT_TRUE(s.onSelected(kIds[2], 40) == SelectorEvent::None);
  TEST_ASSERT_TRUE(s.onSelected(kIds[1], 50) == SelectorEvent::Selected);
  TEST_ASSERT_FALSE(s.isOpen());
  TEST_ASSERT_EQUAL_UINT32(1, s.selections());
}

static void confirming_the_current_activity_just_closes() {
  ActivityCatalog catalog;
  load(catalog, 3, 1);
  ActivitySelector s(10000, 30000, 2000);
  s.open(catalog, 0);
  TEST_ASSERT_TRUE(s.confirm(catalog, 10) == SelectorEvent::Unchanged);
  TEST_ASSERT_FALSE(s.isOpen());
}

static void ten_seconds_without_input_cancels_the_menu() {
  ActivityCatalog catalog;
  load(catalog, 3, 0);
  ActivitySelector s(10000, 30000, 2000);
  s.open(catalog, 1000);
  TEST_ASSERT_TRUE(s.tick(10999) == SelectorEvent::None);
  s.next(catalog, 10000);  // input restarts the idle timer
  TEST_ASSERT_TRUE(s.tick(19999) == SelectorEvent::None);
  TEST_ASSERT_TRUE(s.tick(20000) == SelectorEvent::TimedOut);
  TEST_ASSERT_FALSE(s.isOpen());
  TEST_ASSERT_EQUAL_UINT32(1, s.timeouts());
}

static void a_refused_or_unanswered_selection_fails_then_closes() {
  ActivityCatalog catalog;
  load(catalog, 3, 0);
  {
    ActivitySelector s(10000, 30000, 2000);
    s.open(catalog, 0);
    s.next(catalog, 0);
    s.confirm(catalog, 0);
    TEST_ASSERT_TRUE(s.onError(500) == SelectorEvent::Failed);
    TEST_ASSERT_TRUE(s.isOpen());  // the failure is shown
    TEST_ASSERT_TRUE(s.tick(2499) == SelectorEvent::None);
    TEST_ASSERT_TRUE(s.tick(2500) == SelectorEvent::Dismissed);
    TEST_ASSERT_FALSE(s.isOpen());
  }
  {
    ActivitySelector s(10000, 30000, 2000);
    s.open(catalog, 0);
    s.next(catalog, 0);
    s.confirm(catalog, 100);
    TEST_ASSERT_TRUE(s.tick(30099) == SelectorEvent::None);
    TEST_ASSERT_TRUE(s.tick(30100) == SelectorEvent::Failed);
    TEST_ASSERT_EQUAL_UINT32(1, s.failures());
    // A late confirmation after the timeout is ignored.
    TEST_ASSERT_TRUE(s.onSelected(kIds[1], 30200) == SelectorEvent::None);
  }
}

static void cancel_closes_from_any_open_state() {
  ActivityCatalog catalog;
  load(catalog, 2, 0);
  ActivitySelector s(10000, 30000, 2000);
  TEST_ASSERT_TRUE(s.cancel() == SelectorEvent::None);
  s.open(catalog, 0);
  s.next(catalog, 0);
  s.confirm(catalog, 0);
  TEST_ASSERT_TRUE(s.cancel() == SelectorEvent::Cancelled);
  TEST_ASSERT_FALSE(s.isOpen());
  TEST_ASSERT_TRUE(s.onSelected(kIds[1], 10) == SelectorEvent::None);
}

// --- gate -----------------------------------------------------------------------------------

static void the_menu_is_refused_while_busy_or_offline() {
  TEST_ASSERT_NULL(tth::activityMenuRefusal(idle()));
  const struct {
    bool ActivityMenuConditions::*field;
  } cases[] = {
      {&ActivityMenuConditions::online},       {&ActivityMenuConditions::sessionReady},
      {&ActivityMenuConditions::conversationReady}, {&ActivityMenuConditions::audioIdle},
      {&ActivityMenuConditions::turnIdle},     {&ActivityMenuConditions::bargeInIdle},
      {&ActivityMenuConditions::catalogReady}, {&ActivityMenuConditions::usingGateway},
  };
  for (const auto& c : cases) {
    ActivityMenuConditions conditions = idle();
    conditions.*(c.field) = false;
    TEST_ASSERT_NOT_NULL(tth::activityMenuRefusal(conditions));
  }
}

// --- saved selection --------------------------------------------------------------------------

static void the_selection_is_saved_restored_and_cleared() {
  MemoryStorage storage;
  char id[37];
  {
    ActivityPreference preference(storage);
    TEST_ASSERT_FALSE(preference.load(id, sizeof(id)));
    TEST_ASSERT_EQUAL_STRING("", id);
    TEST_ASSERT_FALSE(preference.save("not-an-id"));
    TEST_ASSERT_TRUE(preference.save(kIds[2]));
  }
  // A reboot: a new object over the same flash.
  {
    ActivityPreference preference(storage);
    TEST_ASSERT_TRUE(preference.load(id, sizeof(id)));
    TEST_ASSERT_EQUAL_STRING(kIds[2], id);
    // What the next hello carries.
    char hello[513];
    TEST_ASSERT_TRUE(tth::wire::encodeHello(hello, sizeof(hello), "core2-6.4", 192000, id) > 0);
    TEST_ASSERT_NOT_NULL(strstr(hello, kIds[2]));
    TEST_ASSERT_TRUE(preference.clear());
    TEST_ASSERT_FALSE(preference.load(id, sizeof(id)));
  }
  // Only the 36-character id is ever stored.
  TEST_ASSERT_EQUAL_UINT32(1, storage.data.size());
  TEST_ASSERT_EQUAL_UINT32(36, storage.data[ActivityPreference::kKey].size());
  // A corrupted value is not used.
  storage.data[ActivityPreference::kKey].assign(36, 'z');
  ActivityPreference preference(storage);
  TEST_ASSERT_FALSE(preference.load(id, sizeof(id)));
  TEST_ASSERT_FALSE(preference.load(id, 36));  // buffer too small
}

// --- display text ------------------------------------------------------------------------------

static void titles_lose_romanian_diacritics_for_the_ascii_font() {
  char out[64];
  tth::toDisplayAscii("Conversa\xC8\x9Bie liber\xC4\x83", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Conversatie libera", out);
  tth::toDisplayAscii("\xC8\x98tiin\xC8\x9B\xC4\x83 \xC3\x8Env\xC4\x83\xC8\x9B\xC4\x83m \xC3\xA2\xC5\x9F\xC5\xA3", out,
                 sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Stiinta Invatam ast", out);
  tth::toDisplayAscii("A\x01" "B\xE2\x82\xAC" "C\xFF", out, sizeof(out));  // control, euro sign, invalid byte
  TEST_ASSERT_EQUAL_STRING("AB?C?", out);
  TEST_ASSERT_EQUAL_UINT32(4, tth::toDisplayAscii("abcdefgh", out, 5));
  TEST_ASSERT_EQUAL_STRING("abcd", out);
  TEST_ASSERT_EQUAL_UINT32(0, tth::toDisplayAscii(nullptr, out, sizeof(out)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(a_list_is_used_only_once_complete);
  RUN_TEST(an_out_of_order_list_is_discarded_and_the_old_one_kept);
  RUN_TEST(a_title_is_stored_bounded_and_never_a_prompt);
  RUN_TEST(the_menu_opens_on_the_current_activity_and_wraps);
  RUN_TEST(an_empty_catalog_never_opens);
  RUN_TEST(confirming_another_activity_requests_it_and_waits);
  RUN_TEST(confirming_the_current_activity_just_closes);
  RUN_TEST(ten_seconds_without_input_cancels_the_menu);
  RUN_TEST(a_refused_or_unanswered_selection_fails_then_closes);
  RUN_TEST(cancel_closes_from_any_open_state);
  RUN_TEST(the_menu_is_refused_while_busy_or_offline);
  RUN_TEST(the_selection_is_saved_restored_and_cleared);
  RUN_TEST(titles_lose_romanian_diacritics_for_the_ascii_font);
  return UNITY_END();
}
