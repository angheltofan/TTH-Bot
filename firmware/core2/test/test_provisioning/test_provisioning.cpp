// Host-side tests for the USB-serial provisioning session: the command flow,
// validation before writing, busy refusal, the confirmed reset, malformed and
// oversized input, and -- throughout -- that no secret ever reaches output.

#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include <unity.h>

#include "tth/ConfigStore.h"
#include "tth/DeviceConfig.h"
#include "tth/LogQueue.h"
#include "tth/Provisioning.h"

using tth::ConfigStore;
using tth::ProvisioningResult;
using tth::ProvisioningSession;

void setUp() {}
void tearDown() {}

namespace {

// Distinctive values, so a leak anywhere in the output is unmistakable.
const std::string kSsid = "SSIDSENTINEL-net";
const std::string kPass = "PASSSENTINEL-9876";
const std::string kToken = "TOKENSENTINELabcdefghijklmnopqrstuvwxyz0123";  // 43
const std::string kUrl = "wss://gw.example.test/v1/ws";

class FakeStorage : public tth::IConfigStorage {
 public:
  FakeStorage() : failWrites(false) {}
  size_t read(const char* key, uint8_t* out, size_t capacity) override {
    const auto it = values.find(key);
    if (it == values.end()) return 0;
    if (it->second.size() <= capacity) memcpy(out, it->second.data(), it->second.size());
    return it->second.size();
  }
  bool write(const char* key, const uint8_t* data, size_t length) override {
    if (failWrites) return false;
    values[key] = std::vector<uint8_t>(data, data + length);
    return true;
  }
  bool eraseAll() override {
    values.clear();
    return true;
  }
  std::map<std::string, std::vector<uint8_t>> values;
  bool failWrites;
};

class FixedRandom : public tth::IRandomSource {
 public:
  explicit FixedRandom(uint32_t value) : _value(value) {}
  uint32_t next() override { return _value; }

 private:
  uint32_t _value;
};

class CaptureSink : public tth::ILineSink {
 public:
  void line(const char* text) override { lines.push_back(text); }
  std::string all() const {
    std::string joined;
    for (const std::string& l : lines) joined += l + "\n";
    return joined;
  }
  bool contains(const std::string& needle) const {
    return all().find(needle) != std::string::npos;
  }
  std::vector<std::string> lines;
};

std::string toHex(const std::string& s, bool upper) {
  static const char* lower = "0123456789abcdef";
  static const char* upperDigits = "0123456789ABCDEF";
  const char* digits = upper ? upperDigits : lower;
  std::string out;
  for (unsigned char c : s) {
    out += digits[c >> 4];
    out += digits[c & 0x0F];
  }
  return out;
}

// Every output line, from every test, goes through this.
void assertNoSecrets(const CaptureSink& sink) {
  const std::string secrets[] = {kSsid, kPass, kToken, "SENTINEL",
                                 toHex(kSsid, false), toHex(kPass, false),
                                 toHex(kToken, false), toHex(kSsid, true),
                                 toHex(kPass, true), toHex(kToken, true)};
  for (const std::string& secret : secrets) {
    if (sink.contains(secret)) {
      TEST_FAIL_MESSAGE(("output contains a secret:\n" + sink.all()).c_str());
    }
  }
}

struct Harness {
  explicit Harness(uint32_t randomValue = 1234)
      : store(storage), random(randomValue), session(store, random) {
    store.load();
  }

  ProvisioningResult send(const std::string& line, bool writesAllowed = true,
                          uint32_t nowMs = 1000) {
    const ProvisioningResult r =
        session.handleLine(line.data(), line.size(), writesAllowed, nowMs, sink);
    assertNoSecrets(sink);
    return r;
  }

  bool lastLineContains(const std::string& needle) const {
    return !sink.lines.empty() && sink.lines.back().find(needle) != std::string::npos;
  }

  void provisionFully() {
    send("prov begin");
    send("prov set ssid " + kSsid);
    send("prov set pass " + kPass);
    send("prov set url " + kUrl);
    send("prov set id core2-01");
    send("prov set token " + kToken);
    const ProvisioningResult r = send("prov commit");
    TEST_ASSERT_TRUE(r.configChanged);
    TEST_ASSERT_TRUE(store.hasConfig());
  }

  FakeStorage storage;
  ConfigStore store;
  FixedRandom random;
  ProvisioningSession session;
  CaptureSink sink;
};

std::string field(const char* data, size_t length) { return std::string(data, length); }

}  // namespace

static void the_full_flow_stores_exact_values_without_echoing_them() {
  Harness h;
  h.provisionFully();
  TEST_ASSERT_TRUE(h.sink.contains("[prov] COMMITTED generation 1 to slot A"));
  const tth::config::DeviceConfig& c = h.store.active();
  TEST_ASSERT_EQUAL_STRING(kSsid.c_str(),
                           field(reinterpret_cast<const char*>(c.ssid), c.ssidLength).c_str());
  TEST_ASSERT_EQUAL_STRING(
      kPass.c_str(), field(reinterpret_cast<const char*>(c.password), c.passwordLength).c_str());
  TEST_ASSERT_EQUAL_STRING(kUrl.c_str(), c.gatewayUrl);
  TEST_ASSERT_EQUAL_STRING("core2-01", c.deviceId);
  TEST_ASSERT_EQUAL_STRING(kToken.c_str(), c.deviceToken);
  TEST_ASSERT_FALSE(h.session.isStaging());
}

static void show_reports_presence_and_safe_metadata_only() {
  Harness h;
  h.provisionFully();
  h.sink.lines.clear();
  const ProvisioningResult r = h.send("prov show");
  TEST_ASSERT_TRUE(r.showRequested);
  TEST_ASSERT_FALSE(r.configChanged);
  TEST_ASSERT_TRUE(h.sink.contains("[config] stored ssid: configured (16 bytes)"));
  TEST_ASSERT_TRUE(h.sink.contains("[config] stored pass: configured\n"));
  TEST_ASSERT_TRUE(h.sink.contains(
      "[config] stored url: configured (wss host=gw.example.test port=443 path=/v1/ws)"));
  TEST_ASSERT_TRUE(h.sink.contains("[config] stored id: configured (core2-01)"));
  TEST_ASSERT_TRUE(h.sink.contains("[config] stored token: configured (43 chars)"));
  TEST_ASSERT_TRUE(h.sink.contains("[config] stored complete: yes"));
  TEST_ASSERT_TRUE(h.sink.contains("[config] stored generation 1 in slot A"));
}

static void show_with_nothing_stored_says_none() {
  Harness h;
  h.send("prov show");
  TEST_ASSERT_TRUE(h.sink.contains("[config] stored: none"));
}

static void show_while_staging_reports_the_missing_fields() {
  Harness h;
  h.send("prov begin");
  h.send("prov set ssid " + kSsid);
  h.sink.lines.clear();
  h.send("prov show");
  TEST_ASSERT_TRUE(h.sink.contains("[config] staged ssid: configured (16 bytes)"));
  TEST_ASSERT_TRUE(h.sink.contains("[config] staged pass: absent"));
  TEST_ASSERT_TRUE(h.sink.contains("[config] staged token: absent"));
  TEST_ASSERT_TRUE(h.sink.contains("[config] staged complete: no (pass: empty)"));
}

// The value is the exact rest of the line, spaces included.
static void values_keep_their_spaces() {
  Harness h;
  h.provisionFully();
  h.send("prov begin");
  h.send("prov set ssid  My Home WiFi ");
  h.send("prov set pass pass phrase with spaces");
  h.send("prov commit");
  const tth::config::DeviceConfig& c = h.store.active();
  TEST_ASSERT_EQUAL_STRING(" My Home WiFi ",
                           field(reinterpret_cast<const char*>(c.ssid), c.ssidLength).c_str());
  TEST_ASSERT_EQUAL_STRING(
      "pass phrase with spaces",
      field(reinterpret_cast<const char*>(c.password), c.passwordLength).c_str());
}

static void sethex_decodes_and_is_never_echoed() {
  Harness h;
  h.send("prov begin");
  h.send("prov sethex ssid " + toHex(kSsid, false));
  TEST_ASSERT_TRUE(h.lastLineContains("[prov] staged ssid: 16 bytes"));
  h.send("prov sethex pass " + toHex(kPass, true));
  TEST_ASSERT_TRUE(h.lastLineContains("[prov] staged pass: set"));
  h.send("prov sethex url " + toHex(kUrl, false));
  h.send("prov sethex id " + toHex("core2-01", false));
  h.send("prov sethex token " + toHex(kToken, false));
  TEST_ASSERT_TRUE(h.send("prov commit").configChanged);
  TEST_ASSERT_EQUAL_STRING(kToken.c_str(), h.store.active().deviceToken);
  TEST_ASSERT_EQUAL_STRING(kPass.c_str(),
                           field(reinterpret_cast<const char*>(h.store.active().password),
                                 h.store.active().passwordLength)
                               .c_str());
}

static void bad_hex_changes_nothing() {
  Harness h;
  h.send("prov begin");
  h.send("prov sethex ssid 414");
  TEST_ASSERT_TRUE(h.lastLineContains("odd length; nothing changed"));
  h.send("prov sethex ssid 41zz");
  TEST_ASSERT_TRUE(h.lastLineContains("not hexadecimal; nothing changed"));
  h.send("prov sethex url " + std::string(2 * (tth::config::kUrlMaxBytes + 1), '4'));
  TEST_ASSERT_TRUE(h.lastLineContains("too long; nothing changed"));
  h.send("prov show");
  TEST_ASSERT_TRUE(h.sink.contains("[config] staged ssid: absent"));
}

static void an_open_network_is_an_explicit_empty_password() {
  Harness h;
  h.send("prov begin");
  h.send("prov set pass");
  TEST_ASSERT_TRUE(h.lastLineContains("[prov] staged pass: set (open network)"));
  h.send("prov set ssid " + kSsid);
  h.send("prov set url " + kUrl);
  h.send("prov set id core2-01");
  h.send("prov set token " + kToken);
  TEST_ASSERT_TRUE(h.send("prov commit").configChanged);
  h.send("prov show");
  TEST_ASSERT_TRUE(h.sink.contains("[config] stored pass: configured (open network)"));
}

static void invalid_values_are_refused_with_the_rule_but_not_the_value() {
  Harness h;
  h.send("prov begin");
  h.send("prov set url ws://gw.example.test/v1/ws");
  TEST_ASSERT_TRUE(h.lastLineContains("ERROR set url: insecure scheme"));
  h.send("prov set pass short");
  TEST_ASSERT_TRUE(h.lastLineContains("ERROR set pass: too short; nothing changed"));
  h.send("prov set token " + kToken.substr(0, 40));
  TEST_ASSERT_TRUE(h.lastLineContains("ERROR set token: wrong length; nothing changed"));
  h.send("prov set ssid " + std::string(33, 'S'));
  TEST_ASSERT_TRUE(h.lastLineContains("ERROR set ssid: too long"));
  h.send("prov set id bad id");
  TEST_ASSERT_TRUE(h.lastLineContains("ERROR set id: invalid character"));
}

// Requirement 6: a malformed command never erases or alters a valid config.
static void malformed_commands_never_touch_the_stored_config() {
  Harness h;
  h.provisionFully();
  const std::vector<uint8_t> slotA = h.storage.values[ConfigStore::kSlotKeyA];
  const char* malformed[] = {
      "",          "prov",           "prov bogus",         "prov set",
      "prov set nope value",         "prov commit now",    "prov reset confirm",
      "prov reset confirm 1234 x",   "prov reset please",  "PROV show",
      "prov  show", "prov show extra", "hello",            "prov abort now",
      "prov begin now"};
  for (const char* line : malformed) {
    const ProvisioningResult r = h.send(line);
    TEST_ASSERT_FALSE(r.configChanged);
  }
  // A commit with nothing staged is refused too.
  TEST_ASSERT_FALSE(h.send("prov commit").configChanged);
  TEST_ASSERT_TRUE(h.lastLineContains("nothing staged"));

  TEST_ASSERT_TRUE(h.store.hasConfig());
  TEST_ASSERT_EQUAL_UINT32(1, h.store.generation());
  TEST_ASSERT_TRUE(h.storage.values[ConfigStore::kSlotKeyA] == slotA);
  TEST_ASSERT_EQUAL_UINT32(1, h.storage.values.size());
}

// A typo can put a secret where a word was expected: it must not be echoed.
static void unknown_input_is_never_echoed() {
  Harness h;
  h.send(kPass);
  TEST_ASSERT_TRUE(h.lastLineContains("commands start with !prov"));
  h.send("prov " + kPass);
  TEST_ASSERT_TRUE(h.lastLineContains("unknown command"));
  h.send("prov begin");
  h.send("prov set " + kToken + " value");
  TEST_ASSERT_TRUE(h.lastLineContains("unknown field"));
  h.send("prov set" + kSsid);
  TEST_ASSERT_TRUE(h.lastLineContains("unknown command"));
}

static void set_without_begin_is_refused() {
  Harness h;
  h.send("prov set ssid " + kSsid);
  TEST_ASSERT_TRUE(h.lastLineContains("send !prov begin first"));
  TEST_ASSERT_FALSE(h.session.isStaging());
}

static void an_incomplete_commit_names_the_field_and_keeps_staging() {
  Harness h;
  h.send("prov begin");
  h.send("prov set ssid " + kSsid);
  h.send("prov set pass " + kPass);
  h.send("prov set url " + kUrl);
  h.send("prov set id core2-01");
  const ProvisioningResult r = h.send("prov commit");
  TEST_ASSERT_FALSE(r.configChanged);
  TEST_ASSERT_TRUE(h.lastLineContains("incomplete (token: empty)"));
  TEST_ASSERT_TRUE(h.session.isStaging());
  TEST_ASSERT_FALSE(h.store.hasConfig());
  h.send("prov set token " + kToken);
  TEST_ASSERT_TRUE(h.send("prov commit").configChanged);
}

// Requirement 8: no flash write lands during a turn or playback.
static void commit_while_busy_is_refused_and_staging_kept() {
  Harness h;
  h.send("prov begin");
  h.send("prov set ssid " + kSsid);
  h.send("prov set pass " + kPass);
  h.send("prov set url " + kUrl);
  h.send("prov set id core2-01");
  h.send("prov set token " + kToken);
  const ProvisioningResult busy = h.send("prov commit", false);
  TEST_ASSERT_FALSE(busy.configChanged);
  TEST_ASSERT_TRUE(h.lastLineContains("busy"));
  TEST_ASSERT_TRUE(h.storage.values.empty());
  TEST_ASSERT_TRUE(h.session.isStaging());
  TEST_ASSERT_TRUE(h.send("prov commit", true).configChanged);
}

static void a_failed_write_keeps_the_previous_generation() {
  Harness h;
  h.provisionFully();
  h.send("prov begin");
  h.send("prov set id core2-02");
  h.storage.failWrites = true;
  const ProvisioningResult r = h.send("prov commit");
  TEST_ASSERT_FALSE(r.configChanged);
  TEST_ASSERT_TRUE(h.lastLineContains("write failed; stored configuration unchanged"));
  TEST_ASSERT_TRUE(h.lastLineContains("previous generation still live"));
  TEST_ASSERT_EQUAL_STRING("core2-01", h.store.active().deviceId);
  TEST_ASSERT_TRUE(h.session.isStaging());
}

static void abort_discards_staging() {
  Harness h;
  h.provisionFully();
  h.send("prov begin");
  h.send("prov set id other");
  h.send("prov abort");
  TEST_ASSERT_TRUE(h.lastLineContains("staging discarded"));
  TEST_ASSERT_FALSE(h.session.isStaging());
  TEST_ASSERT_EQUAL_STRING("core2-01", h.store.active().deviceId);
  h.send("prov abort");
  TEST_ASSERT_TRUE(h.lastLineContains("nothing staged"));
}

static void begin_starts_from_the_stored_config() {
  Harness h;
  h.provisionFully();
  h.send("prov begin");
  TEST_ASSERT_TRUE(h.lastLineContains("from the stored configuration (generation 1)"));
  h.send("prov set id core2-99");
  TEST_ASSERT_TRUE(h.send("prov commit").configChanged);
  TEST_ASSERT_EQUAL_STRING(kToken.c_str(), h.store.active().deviceToken);
  TEST_ASSERT_EQUAL_STRING("core2-99", h.store.active().deviceId);
  TEST_ASSERT_EQUAL_UINT32(2, h.store.generation());
}

// Requirement 7: deliberate, confirmed, namespace-only.
static void reset_requires_the_code_it_issued() {
  Harness h(1234);  // code = 1000 + 1234 % 9000 = 2234
  h.provisionFully();
  TEST_ASSERT_FALSE(h.send("prov reset").configChanged);
  TEST_ASSERT_TRUE(h.lastLineContains("!prov reset confirm 2234"));
  TEST_ASSERT_TRUE(h.lastLineContains("NVS namespace tth only"));
  TEST_ASSERT_TRUE(h.store.hasConfig());
  const ProvisioningResult r = h.send("prov reset confirm 2234", true, 5000);
  TEST_ASSERT_TRUE(r.configChanged);
  TEST_ASSERT_TRUE(h.lastLineContains("RESET done"));
  TEST_ASSERT_FALSE(h.store.hasConfig());
  TEST_ASSERT_TRUE(h.storage.values.empty());
}

static void a_wrong_code_cancels_the_reset() {
  Harness h(1234);
  h.provisionFully();
  h.send("prov reset");
  TEST_ASSERT_FALSE(h.send("prov reset confirm 2235").configChanged);
  TEST_ASSERT_TRUE(h.lastLineContains("wrong confirmation code; reset cancelled"));
  // The right code no longer works: a new request is needed.
  TEST_ASSERT_FALSE(h.send("prov reset confirm 2234").configChanged);
  TEST_ASSERT_TRUE(h.lastLineContains("no reset pending"));
  TEST_ASSERT_FALSE(h.send("prov reset confirm abcd").configChanged);
  TEST_ASSERT_TRUE(h.store.hasConfig());
}

static void a_non_numeric_code_cancels_the_reset() {
  Harness h(1234);
  h.provisionFully();
  h.send("prov reset");
  TEST_ASSERT_FALSE(h.send("prov reset confirm 22a4").configChanged);
  TEST_ASSERT_FALSE(h.session.isResetPending());
  TEST_ASSERT_TRUE(h.store.hasConfig());
}

static void an_expired_code_cancels_the_reset() {
  Harness h(1234);
  h.provisionFully();
  h.send("prov reset", true, 10000);
  TEST_ASSERT_FALSE(
      h.send("prov reset confirm 2234", true,
             10000 + ProvisioningSession::kResetConfirmWindowMs).configChanged);
  TEST_ASSERT_TRUE(h.lastLineContains("expired"));
  TEST_ASSERT_TRUE(h.store.hasConfig());
}

static void reset_while_busy_waits_for_idle() {
  Harness h(1234);
  h.provisionFully();
  h.send("prov reset");
  TEST_ASSERT_FALSE(h.send("prov reset confirm 2234", false).configChanged);
  TEST_ASSERT_TRUE(h.lastLineContains("busy"));
  TEST_ASSERT_TRUE(h.session.isResetPending());
  TEST_ASSERT_TRUE(h.store.hasConfig());
  TEST_ASSERT_TRUE(h.send("prov reset confirm 2234", true).configChanged);
}

static void an_oversized_line_is_refused() {
  Harness h;
  h.provisionFully();
  const std::string huge = "prov set ssid " + std::string(ProvisioningSession::kMaxLineBytes, 'x');
  TEST_ASSERT_FALSE(h.send(huge).configChanged);
  TEST_ASSERT_TRUE(h.lastLineContains("line too long; nothing changed"));
  TEST_ASSERT_EQUAL_UINT32(1, h.store.generation());
}

static void ping_and_help_answer() {
  Harness h;
  h.send("prov ping");
  TEST_ASSERT_TRUE(h.lastLineContains("[prov] pong"));
  h.sink.lines.clear();
  h.send("prov help");
  TEST_ASSERT_TRUE(h.sink.contains("!prov set <field> <value>"));
  TEST_ASSERT_TRUE(h.sink.contains("!prov reset"));
  for (const std::string& line : h.sink.lines) {
    TEST_ASSERT_TRUE(line.size() < tth::kLogMessageMax);
  }
}

// --- Step 6.2: the gateway CA ------------------------------------------------------------

namespace {

const std::string kCaPem = "-----BEGIN CERTIFICATE-----\n"
                           "MIIBszCCAVmgAwIBAgIUQ2VydGlmaWNhdGVGb3JUZXN0czAKBggqhkjOPQQDAjAd\n"
                           "TUlJQ0FGT1JURVNUU09OTFlOT1RBUkVBTENFUlRJRklDQVRF\n"
                           "-----END CERTIFICATE-----\n";

class FakeCertificateCheck : public tth::ICertificateCheck {
 public:
  FakeCertificateCheck() : accept(true), calls(0) {}
  bool check(const char*, size_t, char* summary, size_t capacity) override {
    ++calls;
    snprintf(summary, capacity, "%s",
             accept ? "1 certificate(s), first subject CN=Test CA" : "parse error -0x2180");
    return accept;
  }
  bool accept;
  int calls;
};

struct CaHarness : Harness {
  CaHarness()
      : active(tth::trust::kMaxPemBytes + 1),
        scratchA(tth::TrustAnchorStore::kScratchBytes),
        scratchB(tth::TrustAnchorStore::kScratchBytes),
        staging(tth::trust::kMaxPemBytes + 1),
        trust(storage) {
    trust.attachBuffers(active.data(), scratchA.data(), scratchB.data());
    trust.load();
    session.attachTrustAnchors(trust, staging.data(), staging.size(), &check);
  }

  void stagePem(const std::string& pem) {
    send("prov ca begin");
    size_t start = 0;
    while (start < pem.size()) {
      size_t end = pem.find('\n', start);
      if (end == std::string::npos) end = pem.size();
      if (end > start) send("prov ca line " + pem.substr(start, end - start));
      start = end + 1;
    }
  }

  std::vector<char> active;
  std::vector<uint8_t> scratchA;
  std::vector<uint8_t> scratchB;
  std::vector<char> staging;
  tth::TrustAnchorStore trust;
  FakeCertificateCheck check;
};

}  // namespace

static void a_ca_is_staged_line_by_line_and_committed() {
  CaHarness h;
  h.stagePem(kCaPem);
  TEST_ASSERT_TRUE(h.lastLineContains("[prov] ca line 4 ("));
  const ProvisioningResult r = h.send("prov ca commit");
  TEST_ASSERT_TRUE(r.configChanged);
  TEST_ASSERT_TRUE(
      h.lastLineContains("[prov] CA COMMITTED generation 1 to slot A (1 certificate(s)"));
  TEST_ASSERT_TRUE(h.trust.hasAnchor());
  TEST_ASSERT_EQUAL_STRING(kCaPem.c_str(), h.trust.pem());
  TEST_ASSERT_FALSE(h.session.isCaStaging());
  h.sink.lines.clear();
  h.send("prov show");
  TEST_ASSERT_TRUE(h.sink.contains("[config] stored ca: configured (1 certificate(s), "));
  TEST_ASSERT_TRUE(h.sink.contains("first subject CN=Test CA"));
}

static void show_says_when_no_ca_is_stored() {
  CaHarness h;
  h.send("prov show");
  TEST_ASSERT_TRUE(h.sink.contains("[config] stored ca: absent"));
}

static void ca_commands_need_begin_and_valid_lines() {
  CaHarness h;
  h.send("prov ca line AAAA");
  TEST_ASSERT_TRUE(h.lastLineContains("send !prov ca begin first"));
  h.send("prov ca commit");
  TEST_ASSERT_TRUE(h.lastLineContains("nothing staged"));
  h.send("prov ca begin");
  h.send("prov ca line " + std::string(101, 'A'));
  TEST_ASSERT_TRUE(h.lastLineContains("1-100 characters"));
  h.send("prov ca line bad\tline");
  TEST_ASSERT_TRUE(h.lastLineContains("invalid character"));
  h.send("prov ca bogus");
  TEST_ASSERT_TRUE(h.lastLineContains("unknown ca command"));
  h.send("prov ca");
  TEST_ASSERT_TRUE(h.lastLineContains("use !prov ca begin"));
  TEST_ASSERT_TRUE(h.session.isCaStaging());
  h.send("prov ca abort");
  TEST_ASSERT_FALSE(h.session.isCaStaging());
}

static void an_oversized_ca_is_discarded() {
  CaHarness h;
  h.send("prov ca begin");
  for (int i = 0; i < 41; ++i) h.send("prov ca line " + std::string(100, 'A'));
  TEST_ASSERT_TRUE(h.lastLineContains("larger than 4096 bytes; CA staging discarded"));
  TEST_ASSERT_FALSE(h.session.isCaStaging());
}

static void an_invalid_or_unparseable_ca_leaves_the_stored_one() {
  CaHarness h;
  h.stagePem(kCaPem);
  h.send("prov ca commit");

  h.send("prov ca begin");
  // The marker is split so the repository secret scan does not flag a test
  // sentinel as a real key; the compiled string is the same.
  h.send("prov ca line -----BEGIN " "PRIVATE KEY-----");
  h.send("prov ca line AAAA");
  h.send("prov ca line -----END " "PRIVATE KEY-----");
  TEST_ASSERT_FALSE(h.send("prov ca commit").configChanged);
  TEST_ASSERT_TRUE(h.lastLineContains("PRIVATE KEY"));
  TEST_ASSERT_EQUAL_UINT32(1, h.trust.generation());

  h.check.accept = false;
  h.stagePem(kCaPem);
  TEST_ASSERT_FALSE(h.send("prov ca commit").configChanged);
  TEST_ASSERT_TRUE(h.lastLineContains("refused by the TLS parser (parse error -0x2180)"));
  TEST_ASSERT_EQUAL_UINT32(1, h.trust.generation());
  TEST_ASSERT_TRUE(h.session.isCaStaging());
}

static void ca_writes_wait_for_idle() {
  CaHarness h;
  h.stagePem(kCaPem);
  TEST_ASSERT_FALSE(h.send("prov ca commit", false).configChanged);
  TEST_ASSERT_TRUE(h.lastLineContains("busy"));
  TEST_ASSERT_TRUE(h.session.isCaStaging());
  TEST_ASSERT_TRUE(h.send("prov ca commit", true).configChanged);
  TEST_ASSERT_FALSE(h.send("prov ca clear", false).configChanged);
  TEST_ASSERT_TRUE(h.trust.hasAnchor());
}

static void ca_clear_disables_the_gateway() {
  CaHarness h;
  h.stagePem(kCaPem);
  h.send("prov ca commit");
  TEST_ASSERT_TRUE(h.send("prov ca clear").configChanged);
  TEST_ASSERT_TRUE(h.lastLineContains("CA CLEARED"));
  TEST_ASSERT_FALSE(h.trust.hasAnchor());
}

static void reset_erases_the_ca_too() {
  CaHarness h;
  h.provisionFully();
  h.stagePem(kCaPem);
  h.send("prov ca commit");
  h.send("prov reset");
  TEST_ASSERT_TRUE(h.send("prov reset confirm 2234").configChanged);
  TEST_ASSERT_FALSE(h.trust.hasAnchor());
  TEST_ASSERT_TRUE(h.storage.values.empty());
}

static void ca_commands_are_unavailable_without_a_store() {
  Harness h;
  h.send("prov ca begin");
  TEST_ASSERT_TRUE(h.lastLineContains("not available"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(a_ca_is_staged_line_by_line_and_committed);
  RUN_TEST(show_says_when_no_ca_is_stored);
  RUN_TEST(ca_commands_need_begin_and_valid_lines);
  RUN_TEST(an_oversized_ca_is_discarded);
  RUN_TEST(an_invalid_or_unparseable_ca_leaves_the_stored_one);
  RUN_TEST(ca_writes_wait_for_idle);
  RUN_TEST(ca_clear_disables_the_gateway);
  RUN_TEST(reset_erases_the_ca_too);
  RUN_TEST(ca_commands_are_unavailable_without_a_store);
  RUN_TEST(the_full_flow_stores_exact_values_without_echoing_them);
  RUN_TEST(show_reports_presence_and_safe_metadata_only);
  RUN_TEST(show_with_nothing_stored_says_none);
  RUN_TEST(show_while_staging_reports_the_missing_fields);
  RUN_TEST(values_keep_their_spaces);
  RUN_TEST(sethex_decodes_and_is_never_echoed);
  RUN_TEST(bad_hex_changes_nothing);
  RUN_TEST(an_open_network_is_an_explicit_empty_password);
  RUN_TEST(invalid_values_are_refused_with_the_rule_but_not_the_value);
  RUN_TEST(malformed_commands_never_touch_the_stored_config);
  RUN_TEST(unknown_input_is_never_echoed);
  RUN_TEST(set_without_begin_is_refused);
  RUN_TEST(an_incomplete_commit_names_the_field_and_keeps_staging);
  RUN_TEST(commit_while_busy_is_refused_and_staging_kept);
  RUN_TEST(a_failed_write_keeps_the_previous_generation);
  RUN_TEST(abort_discards_staging);
  RUN_TEST(begin_starts_from_the_stored_config);
  RUN_TEST(reset_requires_the_code_it_issued);
  RUN_TEST(a_wrong_code_cancels_the_reset);
  RUN_TEST(a_non_numeric_code_cancels_the_reset);
  RUN_TEST(an_expired_code_cancels_the_reset);
  RUN_TEST(reset_while_busy_waits_for_idle);
  RUN_TEST(an_oversized_line_is_refused);
  RUN_TEST(ping_and_help_answer);
  return UNITY_END();
}
