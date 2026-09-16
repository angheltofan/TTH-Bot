// Host-side tests for ConfigStore: the A/B transactional commit.
//
// The fake storage can fail, tear or corrupt a write, or fail an erase, so
// every way a commit can go wrong is shown to leave the last valid
// configuration in place -- both in memory and after a simulated reboot.

#include <string.h>

#include <map>
#include <string>
#include <vector>

#include <unity.h>

#include "tth/ConfigStore.h"
#include "tth/DeviceConfig.h"

using tth::CommitResult;
using tth::ConfigStore;
using tth::config::Check;
using tth::config::DeviceConfig;
using tth::config::Field;
using tth::config::RecordStatus;

void setUp() {}
void tearDown() {}

namespace {

enum class WriteFault { None, Fail, TornFail, TornOk, Corrupt };

class FakeStorage : public tth::IConfigStorage {
 public:
  FakeStorage() : fault(WriteFault::None), eraseFails(false), writes(0) {}

  size_t read(const char* key, uint8_t* out, size_t capacity) override {
    const auto it = values.find(key);
    if (it == values.end()) return 0;
    if (it->second.size() <= capacity && out != nullptr) {
      memcpy(out, it->second.data(), it->second.size());
    }
    return it->second.size();
  }

  bool write(const char* key, const uint8_t* data, size_t length) override {
    ++writes;
    writtenKeys.push_back(key);
    std::vector<uint8_t> bytes(data, data + length);
    switch (fault) {
      case WriteFault::None:
        values[key] = bytes;
        return true;
      case WriteFault::Fail:
        return false;
      case WriteFault::TornFail:  // power lost mid-write
        bytes.resize(length / 2);
        values[key] = bytes;
        return false;
      case WriteFault::TornOk:  // flash lied about success
        bytes.resize(length / 2);
        values[key] = bytes;
        return true;
      case WriteFault::Corrupt:
        bytes[length / 2] ^= 0x40;
        values[key] = bytes;
        return true;
    }
    return false;
  }

  bool eraseAll() override {
    if (eraseFails) return false;
    values.clear();
    return true;
  }

  std::map<std::string, std::vector<uint8_t>> values;
  WriteFault fault;
  bool eraseFails;
  int writes;
  std::vector<std::string> writtenKeys;
};

const char* const kToken = "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFG";  // 43

DeviceConfig configWithId(const char* id) {
  DeviceConfig c;
  tth::config::clear(c);
  const char* values[5] = {"Classroom", "correct-horse", "wss://gw.example.test/v1/ws",
                           id, kToken};
  for (int i = 0; i < 5; ++i) {
    TEST_ASSERT_TRUE(tth::config::setField(c, static_cast<Field>(i),
                                           reinterpret_cast<const uint8_t*>(values[i]),
                                           strlen(values[i]), nullptr));
  }
  return c;
}

std::string idOf(const DeviceConfig& c) { return std::string(c.deviceId, c.deviceIdLength); }

void storeRaw(FakeStorage& storage, const char* key, const DeviceConfig& c,
              uint32_t generation) {
  uint8_t buffer[tth::config::kRecordMaxBytes];
  const size_t n = tth::config::encodeRecord(c, generation, buffer, sizeof(buffer));
  TEST_ASSERT_TRUE(n > 0);
  storage.values[key] = std::vector<uint8_t>(buffer, buffer + n);
}

// A "reboot": a brand-new store over the same flash.
std::string reloadedId(FakeStorage& storage) {
  ConfigStore store(storage);
  store.load();
  return store.hasConfig() ? idOf(store.active()) : std::string("<none>");
}

}  // namespace

static void an_empty_flash_loads_nothing() {
  FakeStorage storage;
  ConfigStore store(storage);
  const tth::LoadReport report = store.load();
  TEST_ASSERT_FALSE(store.hasConfig());
  TEST_ASSERT_TRUE(report.a.status == RecordStatus::Absent);
  TEST_ASSERT_TRUE(report.b.status == RecordStatus::Absent);
  TEST_ASSERT_EQUAL_INT(0, report.chosen);
  TEST_ASSERT_EQUAL_INT(0, store.activeSlot());
}

static void a_commit_persists_across_a_reboot() {
  FakeStorage storage;
  ConfigStore store(storage);
  store.load();
  TEST_ASSERT_TRUE(store.commit(configWithId("first"), nullptr, nullptr) == CommitResult::Ok);
  TEST_ASSERT_TRUE(store.hasConfig());
  TEST_ASSERT_EQUAL_UINT32(1, store.generation());
  TEST_ASSERT_EQUAL_INT('A', store.activeSlot());

  ConfigStore rebooted(storage);
  const tth::LoadReport report = rebooted.load();
  TEST_ASSERT_TRUE(rebooted.hasConfig());
  TEST_ASSERT_EQUAL_INT('A', report.chosen);
  TEST_ASSERT_EQUAL_UINT32(1, report.generation);
  const DeviceConfig expected = configWithId("first");
  TEST_ASSERT_EQUAL_MEMORY(&expected, &rebooted.active(), sizeof(DeviceConfig));
}

// The slot holding the live configuration is never written by a commit.
static void commits_alternate_slots_and_never_touch_the_live_one() {
  FakeStorage storage;
  ConfigStore store(storage);
  store.load();
  const char* ids[4] = {"one", "two", "three", "four"};
  const char expectedSlot[4] = {'A', 'B', 'A', 'B'};
  for (int i = 0; i < 4; ++i) {
    const char live = store.activeSlot();
    TEST_ASSERT_TRUE(store.commit(configWithId(ids[i]), nullptr, nullptr) == CommitResult::Ok);
    TEST_ASSERT_EQUAL_INT(expectedSlot[i], store.activeSlot());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(i + 1), store.generation());
    const std::string written = storage.writtenKeys.back();
    if (live == 'A') TEST_ASSERT_EQUAL_STRING(ConfigStore::kSlotKeyB, written.c_str());
    if (live == 'B') TEST_ASSERT_EQUAL_STRING(ConfigStore::kSlotKeyA, written.c_str());
    TEST_ASSERT_EQUAL_STRING(ids[i], reloadedId(storage).c_str());
  }
}

static void an_incomplete_config_writes_nothing() {
  FakeStorage storage;
  ConfigStore store(storage);
  store.load();
  store.commit(configWithId("good"), nullptr, nullptr);
  const int writesBefore = storage.writes;

  DeviceConfig partial = configWithId("partial");
  partial.presentMask = static_cast<uint8_t>(partial.presentMask & ~(1u << 4));
  Field problem = Field::Ssid;
  Check why = Check::Ok;
  TEST_ASSERT_TRUE(store.commit(partial, &problem, &why) == CommitResult::Incomplete);
  TEST_ASSERT_TRUE(problem == Field::DeviceToken);
  TEST_ASSERT_TRUE(why == Check::Empty);
  TEST_ASSERT_EQUAL_INT(writesBefore, storage.writes);
  TEST_ASSERT_EQUAL_STRING("good", idOf(store.active()).c_str());
}

static void check_fault_preserves_previous(WriteFault fault, CommitResult expected) {
  FakeStorage storage;
  ConfigStore store(storage);
  store.load();
  TEST_ASSERT_TRUE(store.commit(configWithId("good"), nullptr, nullptr) == CommitResult::Ok);

  storage.fault = fault;
  TEST_ASSERT_TRUE(store.commit(configWithId("broken"), nullptr, nullptr) == expected);
  // In memory: unchanged.
  TEST_ASSERT_TRUE(store.hasConfig());
  TEST_ASSERT_EQUAL_STRING("good", idOf(store.active()).c_str());
  TEST_ASSERT_EQUAL_UINT32(1, store.generation());
  TEST_ASSERT_EQUAL_INT('A', store.activeSlot());
  // After a reboot: unchanged.
  TEST_ASSERT_EQUAL_STRING("good", reloadedId(storage).c_str());

  // And the store recovers once the flash behaves again.
  storage.fault = WriteFault::None;
  TEST_ASSERT_TRUE(store.commit(configWithId("fixed"), nullptr, nullptr) == CommitResult::Ok);
  TEST_ASSERT_EQUAL_UINT32(2, store.generation());
  TEST_ASSERT_EQUAL_STRING("fixed", reloadedId(storage).c_str());
}

static void a_failed_write_keeps_the_previous_config() {
  check_fault_preserves_previous(WriteFault::Fail, CommitResult::WriteFailed);
}

static void an_interrupted_write_keeps_the_previous_config() {
  check_fault_preserves_previous(WriteFault::TornFail, CommitResult::WriteFailed);
}

static void a_torn_write_reported_as_success_is_caught_by_verification() {
  check_fault_preserves_previous(WriteFault::TornOk, CommitResult::VerifyFailed);
}

static void a_corrupted_write_is_caught_by_verification() {
  check_fault_preserves_previous(WriteFault::Corrupt, CommitResult::VerifyFailed);
}

static void a_failure_with_nothing_stored_leaves_nothing_stored() {
  FakeStorage storage;
  ConfigStore store(storage);
  store.load();
  storage.fault = WriteFault::TornFail;
  TEST_ASSERT_TRUE(store.commit(configWithId("x"), nullptr, nullptr) == CommitResult::WriteFailed);
  TEST_ASSERT_FALSE(store.hasConfig());
  TEST_ASSERT_EQUAL_STRING("<none>", reloadedId(storage).c_str());
}

static void the_highest_valid_generation_wins_on_boot() {
  FakeStorage storage;
  storeRaw(storage, ConfigStore::kSlotKeyA, configWithId("older"), 6);
  storeRaw(storage, ConfigStore::kSlotKeyB, configWithId("newer"), 7);
  ConfigStore store(storage);
  const tth::LoadReport report = store.load();
  TEST_ASSERT_EQUAL_INT('B', report.chosen);
  TEST_ASSERT_EQUAL_STRING("newer", idOf(store.active()).c_str());

  // A newer next commit goes to A (not the live slot) with generation 8.
  TEST_ASSERT_TRUE(store.commit(configWithId("next"), nullptr, nullptr) == CommitResult::Ok);
  TEST_ASSERT_EQUAL_INT('A', store.activeSlot());
  TEST_ASSERT_EQUAL_UINT32(8, store.generation());
}

static void a_damaged_newer_slot_falls_back_to_the_older_valid_one() {
  FakeStorage storage;
  storeRaw(storage, ConfigStore::kSlotKeyA, configWithId("older"), 3);
  storeRaw(storage, ConfigStore::kSlotKeyB, configWithId("newer"), 4);
  storage.values[ConfigStore::kSlotKeyB][20] ^= 0xFF;
  ConfigStore store(storage);
  const tth::LoadReport report = store.load();
  TEST_ASSERT_TRUE(report.b.status == RecordStatus::BadCrc);
  TEST_ASSERT_EQUAL_INT('A', report.chosen);
  TEST_ASSERT_EQUAL_STRING("older", idOf(store.active()).c_str());
}

static void an_oversized_stored_value_is_rejected_without_overflow() {
  FakeStorage storage;
  storage.values[ConfigStore::kSlotKeyA] =
      std::vector<uint8_t>(tth::config::kRecordMaxBytes + 100, 0xAA);
  storage.values[ConfigStore::kSlotKeyB] = std::vector<uint8_t>(3, 0x01);
  ConfigStore store(storage);
  const tth::LoadReport report = store.load();
  TEST_ASSERT_TRUE(report.a.status == RecordStatus::BadLength);
  TEST_ASSERT_TRUE(report.b.status == RecordStatus::TooShort);
  TEST_ASSERT_FALSE(store.hasConfig());
}

static void an_exhausted_generation_counter_refuses_to_wrap() {
  FakeStorage storage;
  storeRaw(storage, ConfigStore::kSlotKeyA, configWithId("last"), 0xFFFFFFFFu);
  ConfigStore store(storage);
  store.load();
  const int writesBefore = storage.writes;
  TEST_ASSERT_TRUE(store.commit(configWithId("wrap"), nullptr, nullptr) ==
                   CommitResult::GenerationExhausted);
  TEST_ASSERT_EQUAL_INT(writesBefore, storage.writes);
  TEST_ASSERT_EQUAL_STRING("last", idOf(store.active()).c_str());
  // A reset is the way out.
  TEST_ASSERT_TRUE(store.reset());
  TEST_ASSERT_TRUE(store.commit(configWithId("fresh"), nullptr, nullptr) == CommitResult::Ok);
  TEST_ASSERT_EQUAL_UINT32(1, store.generation());
}

static void reset_erases_both_slots_and_the_live_copy() {
  FakeStorage storage;
  ConfigStore store(storage);
  store.load();
  store.commit(configWithId("one"), nullptr, nullptr);
  store.commit(configWithId("two"), nullptr, nullptr);
  TEST_ASSERT_TRUE(store.reset());
  TEST_ASSERT_FALSE(store.hasConfig());
  TEST_ASSERT_EQUAL_UINT32(0, store.generation());
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&store.active());
  for (size_t i = 0; i < sizeof(DeviceConfig); ++i) TEST_ASSERT_EQUAL_UINT8(0, p[i]);
  TEST_ASSERT_TRUE(storage.values.empty());
  TEST_ASSERT_EQUAL_STRING("<none>", reloadedId(storage).c_str());
}

static void a_failed_erase_keeps_the_config() {
  FakeStorage storage;
  ConfigStore store(storage);
  store.load();
  store.commit(configWithId("kept"), nullptr, nullptr);
  storage.eraseFails = true;
  TEST_ASSERT_FALSE(store.reset());
  TEST_ASSERT_TRUE(store.hasConfig());
  TEST_ASSERT_EQUAL_STRING("kept", idOf(store.active()).c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(an_empty_flash_loads_nothing);
  RUN_TEST(a_commit_persists_across_a_reboot);
  RUN_TEST(commits_alternate_slots_and_never_touch_the_live_one);
  RUN_TEST(an_incomplete_config_writes_nothing);
  RUN_TEST(a_failed_write_keeps_the_previous_config);
  RUN_TEST(an_interrupted_write_keeps_the_previous_config);
  RUN_TEST(a_torn_write_reported_as_success_is_caught_by_verification);
  RUN_TEST(a_corrupted_write_is_caught_by_verification);
  RUN_TEST(a_failure_with_nothing_stored_leaves_nothing_stored);
  RUN_TEST(the_highest_valid_generation_wins_on_boot);
  RUN_TEST(a_damaged_newer_slot_falls_back_to_the_older_valid_one);
  RUN_TEST(an_oversized_stored_value_is_rejected_without_overflow);
  RUN_TEST(an_exhausted_generation_counter_refuses_to_wrap);
  RUN_TEST(reset_erases_both_slots_and_the_live_copy);
  RUN_TEST(a_failed_erase_keeps_the_config);
  return UNITY_END();
}
