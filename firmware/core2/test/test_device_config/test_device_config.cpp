// Host-side tests for DeviceConfig: field validation, the gateway URL rules,
// and the stored record format.

#include <string.h>

#include <string>

#include <unity.h>

#include "tth/DeviceConfig.h"

using tth::config::Check;
using tth::config::DeviceConfig;
using tth::config::Field;
using tth::config::RecordStatus;

void setUp() {}
void tearDown() {}

namespace {

const std::string kToken = "TOKENSENTINELabcdefghijklmnopqrstuvwxyz0123";  // 43

const uint8_t* bytesOf(const std::string& s) {
  return reinterpret_cast<const uint8_t*>(s.data());
}

Check ssid(const std::string& s) { return tth::config::checkSsid(bytesOf(s), s.size()); }
Check pass(const std::string& s) { return tth::config::checkPassword(bytesOf(s), s.size()); }
Check url(const std::string& s) {
  return tth::config::checkGatewayUrl(s.data(), s.size(), nullptr);
}
Check deviceId(const std::string& s) { return tth::config::checkDeviceId(s.data(), s.size()); }
Check token(const std::string& s) {
  return tth::config::checkDeviceToken(s.data(), s.size());
}

DeviceConfig complete() {
  DeviceConfig c;
  tth::config::clear(c);
  const std::string values[5] = {"Classroom WiFi", "hunter2-hunter2",
                                 "wss://gw.example.test/v1/ws", "core2-01", kToken};
  for (int i = 0; i < 5; ++i) {
    Check why = Check::Ok;
    TEST_ASSERT_TRUE(tth::config::setField(c, static_cast<Field>(i), bytesOf(values[i]),
                                           values[i].size(), &why));
  }
  return c;
}

}  // namespace

// --- fields ---------------------------------------------------------------------------

static void ssid_lengths_and_characters() {
  TEST_ASSERT_TRUE(ssid("") == Check::Empty);
  TEST_ASSERT_TRUE(ssid("a") == Check::Ok);
  TEST_ASSERT_TRUE(ssid(std::string(32, 'x')) == Check::Ok);
  TEST_ASSERT_TRUE(ssid(std::string(33, 'x')) == Check::TooLong);
  TEST_ASSERT_TRUE(ssid("with spaces ok") == Check::Ok);
  TEST_ASSERT_TRUE(ssid("Caf\xC3\xA9") == Check::Ok);  // UTF-8
  TEST_ASSERT_TRUE(ssid(std::string("a\x01b", 3)) == Check::BadCharacter);
  TEST_ASSERT_TRUE(ssid(std::string("a\x00b", 3)) == Check::BadCharacter);
  TEST_ASSERT_TRUE(ssid("tab\there") == Check::BadCharacter);
  TEST_ASSERT_TRUE(ssid("del\x7f") == Check::BadCharacter);
}

static void password_rules() {
  TEST_ASSERT_TRUE(pass("") == Check::Ok);  // open network
  TEST_ASSERT_TRUE(pass("1234567") == Check::TooShort);
  TEST_ASSERT_TRUE(pass("12345678") == Check::Ok);
  TEST_ASSERT_TRUE(pass(std::string(63, 'p')) == Check::Ok);
  TEST_ASSERT_TRUE(pass(std::string(64, 'a')) == Check::Ok);  // raw PSK, hex
  TEST_ASSERT_TRUE(pass(std::string(63, 'a') + "Z") == Check::NotHex);
  TEST_ASSERT_TRUE(pass(std::string(65, 'a')) == Check::TooLong);
  TEST_ASSERT_TRUE(pass("with space ok") == Check::Ok);
  TEST_ASSERT_TRUE(pass("tab\tinside") == Check::BadCharacter);
  TEST_ASSERT_TRUE(pass("nonascii\xC3\xA9") == Check::BadCharacter);
}

static void device_id_rules() {
  TEST_ASSERT_TRUE(deviceId("") == Check::Empty);
  TEST_ASSERT_TRUE(deviceId("core2-01_A") == Check::Ok);
  TEST_ASSERT_TRUE(deviceId(std::string(32, 'a')) == Check::Ok);
  TEST_ASSERT_TRUE(deviceId(std::string(33, 'a')) == Check::TooLong);
  TEST_ASSERT_TRUE(deviceId("core2 01") == Check::BadCharacter);
  TEST_ASSERT_TRUE(deviceId("core2.01") == Check::BadCharacter);
}

static void token_rules() {
  TEST_ASSERT_TRUE(token("") == Check::Empty);
  TEST_ASSERT_TRUE(token(kToken) == Check::Ok);
  TEST_ASSERT_TRUE(token(kToken.substr(0, 42)) == Check::BadLength);
  TEST_ASSERT_TRUE(token(kToken + "A") == Check::BadLength);
  std::string bad = kToken;
  bad[5] = '+';  // standard base64, not base64url
  TEST_ASSERT_TRUE(token(bad) == Check::BadCharacter);
}

// --- gateway URL ----------------------------------------------------------------------------

static void url_accepts_wss_and_reports_its_parts() {
  tth::config::GatewayUrlParts parts;
  const std::string u = "wss://gw.example.test:8443/v1/ws";
  TEST_ASSERT_TRUE(tth::config::checkGatewayUrl(u.data(), u.size(), &parts) == Check::Ok);
  TEST_ASSERT_EQUAL_STRING("gw.example.test", parts.host);
  TEST_ASSERT_EQUAL_UINT16(8443, parts.port);
  TEST_ASSERT_EQUAL_STRING("/v1/ws", parts.path);
  TEST_ASSERT_FALSE(parts.isIpv4);

  const std::string lan = "wss://192.168.1.20/v1/ws";
  TEST_ASSERT_TRUE(tth::config::checkGatewayUrl(lan.data(), lan.size(), &parts) == Check::Ok);
  TEST_ASSERT_EQUAL_UINT16(443, parts.port);  // default
  TEST_ASSERT_TRUE(parts.isIpv4);

  TEST_ASSERT_TRUE(url("wss://host/") == Check::Ok);
}

// Production must be TLS: only wss:// is approved by the plan.
static void url_refuses_every_scheme_but_wss() {
  TEST_ASSERT_TRUE(url("ws://gw.example.test/v1/ws") == Check::InsecureScheme);
  TEST_ASSERT_TRUE(url("https://gw.example.test/v1/ws") == Check::UnsupportedScheme);
  TEST_ASSERT_TRUE(url("http://gw.example.test/v1/ws") == Check::UnsupportedScheme);
  TEST_ASSERT_TRUE(url("WSS://gw.example.test/v1/ws") == Check::UnsupportedScheme);
  TEST_ASSERT_TRUE(url("gw.example.test/v1/ws") == Check::UnsupportedScheme);
}

static void url_refuses_userinfo_query_fragment_and_whitespace() {
  TEST_ASSERT_TRUE(url("wss://user:pw@gw.example.test/v1/ws") == Check::UserInfoNotAllowed);
  TEST_ASSERT_TRUE(url("wss://gw.example.test/v1/ws?token=x") == Check::QueryNotAllowed);
  TEST_ASSERT_TRUE(url("wss://gw.example.test/v1/ws#x") == Check::QueryNotAllowed);
  TEST_ASSERT_TRUE(url("wss://gw.example.test/v1 ws") == Check::BadCharacter);
  TEST_ASSERT_TRUE(url("wss://gw.example.test/v1/ws\n") == Check::BadCharacter);
  TEST_ASSERT_TRUE(url("") == Check::Empty);
}

static void url_host_and_port_rules() {
  TEST_ASSERT_TRUE(url("wss:///v1/ws") == Check::BadHost);
  TEST_ASSERT_TRUE(url("wss://[::1]/v1/ws") == Check::BadHost);
  TEST_ASSERT_TRUE(url("wss://" + std::string(64, 'a') + ".test/v1/ws") == Check::BadHost);
  TEST_ASSERT_TRUE(url("wss://-bad.test/v1/ws") == Check::BadHost);
  TEST_ASSERT_TRUE(url("wss://bad-.test/v1/ws") == Check::BadHost);
  TEST_ASSERT_TRUE(url("wss://a..b/v1/ws") == Check::BadHost);
  TEST_ASSERT_TRUE(url("wss://trailing.dot./v1/ws") == Check::BadHost);
  TEST_ASSERT_TRUE(url("wss://under_score.test/v1/ws") == Check::BadHost);
  TEST_ASSERT_TRUE(url("wss://256.1.1.1/v1/ws") == Check::BadHost);
  TEST_ASSERT_TRUE(url("wss://1.2.3/v1/ws") == Check::BadHost);
  TEST_ASSERT_TRUE(url("wss://1.2.3.4.5/v1/ws") == Check::BadHost);
  TEST_ASSERT_TRUE(url("wss://host:0/v1/ws") == Check::BadPort);
  TEST_ASSERT_TRUE(url("wss://host:65536/v1/ws") == Check::BadPort);
  TEST_ASSERT_TRUE(url("wss://host:65535/v1/ws") == Check::Ok);
  TEST_ASSERT_TRUE(url("wss://host:/v1/ws") == Check::BadPort);
  TEST_ASSERT_TRUE(url("wss://host:44a/v1/ws") == Check::BadPort);
  TEST_ASSERT_TRUE(url("wss://host:123456/v1/ws") == Check::BadPort);
  TEST_ASSERT_TRUE(url("wss://host:1:2/v1/ws") == Check::BadHost);
}

static void url_path_and_length_rules() {
  TEST_ASSERT_TRUE(url("wss://gw.example.test") == Check::MissingPath);
  TEST_ASSERT_TRUE(url("wss://gw.example.test/v1/%2e") == Check::BadPath);
  TEST_ASSERT_TRUE(url("wss://h/" + std::string(180, 'p')) == Check::BadPath);
  TEST_ASSERT_TRUE(url("wss://h/" + std::string(179, 'p')) == Check::Ok);
  const std::string longest = "wss://h/" + std::string(192, 'p');  // 200, path 193
  TEST_ASSERT_EQUAL_UINT32(200, longest.size());
  TEST_ASSERT_TRUE(url(longest) == Check::BadPath);
  TEST_ASSERT_TRUE(url("wss://" + std::string(193, 'h') + "/") == Check::BadHost);
  TEST_ASSERT_TRUE(url("wss://h/" + std::string(193, 'p')) == Check::TooLong);
}

// --- config ---------------------------------------------------------------------------------

static void a_rejected_value_leaves_the_field_exactly_as_it_was() {
  DeviceConfig c = complete();
  const DeviceConfig before = c;
  Check why = Check::Ok;
  const std::string insecure = "ws://gw.example.test/v1/ws";
  TEST_ASSERT_FALSE(tth::config::setField(c, Field::GatewayUrl, bytesOf(insecure),
                                          insecure.size(), &why));
  TEST_ASSERT_TRUE(why == Check::InsecureScheme);
  TEST_ASSERT_EQUAL_MEMORY(&before, &c, sizeof(c));
}

static void completeness_names_the_first_missing_field() {
  DeviceConfig c;
  tth::config::clear(c);
  Field problem = Field::DeviceToken;
  TEST_ASSERT_TRUE(tth::config::checkComplete(c, &problem) == Check::Empty);
  TEST_ASSERT_TRUE(problem == Field::Ssid);
  c = complete();
  TEST_ASSERT_TRUE(tth::config::checkComplete(c, &problem) == Check::Ok);
  // An open network is complete: the password is present but empty.
  Check why = Check::Ok;
  TEST_ASSERT_TRUE(tth::config::setField(c, Field::Password, nullptr, 0, &why));
  TEST_ASSERT_TRUE(tth::config::checkComplete(c, &problem) == Check::Ok);
}

static void clear_zeroes_every_byte() {
  DeviceConfig c = complete();
  tth::config::clear(c);
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&c);
  for (size_t i = 0; i < sizeof(c); ++i) TEST_ASSERT_EQUAL_UINT8(0, p[i]);
}

// --- record -----------------------------------------------------------------------------------

static void crc32_matches_the_standard_check_value() {
  const char* check = "123456789";
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u,
                          tth::config::crc32(reinterpret_cast<const uint8_t*>(check), 9));
}

static void a_record_round_trips() {
  const DeviceConfig c = complete();
  uint8_t buffer[tth::config::kRecordMaxBytes];
  const size_t n = tth::config::encodeRecord(c, 42, buffer, sizeof(buffer));
  TEST_ASSERT_TRUE(n > 0);
  DeviceConfig out;
  uint32_t generation = 0;
  TEST_ASSERT_TRUE(tth::config::decodeRecord(buffer, n, out, generation) == RecordStatus::Ok);
  TEST_ASSERT_EQUAL_UINT32(42, generation);
  TEST_ASSERT_EQUAL_MEMORY(&c, &out, sizeof(c));
}

static void the_largest_record_fits_the_maximum() {
  DeviceConfig c;
  tth::config::clear(c);
  const std::string values[5] = {std::string(32, 's'), std::string(64, 'a'),
                                 "wss://h/" + std::string(172, 'p'),
                                 std::string(32, 'i'), kToken};
  for (int i = 0; i < 5; ++i) {
    TEST_ASSERT_TRUE(tth::config::setField(c, static_cast<Field>(i), bytesOf(values[i]),
                                           values[i].size(), nullptr));
  }
  uint8_t buffer[tth::config::kRecordMaxBytes];
  TEST_ASSERT_TRUE(tth::config::encodeRecord(c, 1, buffer, sizeof(buffer)) > 0);
  TEST_ASSERT_EQUAL_UINT32(0, tth::config::encodeRecord(c, 1, buffer, 100));
}

static void corrupted_records_are_rejected() {
  const DeviceConfig c = complete();
  uint8_t good[tth::config::kRecordMaxBytes];
  const size_t n = tth::config::encodeRecord(c, 7, good, sizeof(good));
  DeviceConfig out;
  uint32_t generation = 0;

  TEST_ASSERT_TRUE(tth::config::decodeRecord(good, 0, out, generation) == RecordStatus::Absent);
  TEST_ASSERT_TRUE(tth::config::decodeRecord(good, 10, out, generation) == RecordStatus::TooShort);

  // Every single-byte flip is caught.
  for (size_t i = 0; i < n; ++i) {
    uint8_t copy[tth::config::kRecordMaxBytes];
    memcpy(copy, good, n);
    copy[i] ^= 0x01;
    const RecordStatus status = tth::config::decodeRecord(copy, n, out, generation);
    if (status == RecordStatus::Ok) TEST_FAIL_MESSAGE("a flipped byte was accepted");
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&out);
    for (size_t k = 0; k < sizeof(out); ++k) {
      if (p[k] != 0) TEST_FAIL_MESSAGE("a rejected record left data behind");
    }
  }
  // Truncated at every length.
  for (size_t len = 1; len < n; ++len) {
    if (tth::config::decodeRecord(good, len, out, generation) == RecordStatus::Ok) {
      TEST_FAIL_MESSAGE("a truncated record was accepted");
    }
  }
}

// A record with a valid checksum but an invalid value is still refused: every
// field is re-validated on load.
static void a_valid_checksum_does_not_bypass_field_validation() {
  const DeviceConfig c = complete();
  uint8_t buffer[tth::config::kRecordMaxBytes];
  const size_t n = tth::config::encodeRecord(c, 1, buffer, sizeof(buffer));
  // The token is the last field: its first byte sits 43 + 4 bytes from the end.
  buffer[n - 4 - 43] = '+';
  const uint32_t crc = tth::config::crc32(buffer, n - 4);
  buffer[n - 4] = static_cast<uint8_t>(crc & 0xFF);
  buffer[n - 3] = static_cast<uint8_t>((crc >> 8) & 0xFF);
  buffer[n - 2] = static_cast<uint8_t>((crc >> 16) & 0xFF);
  buffer[n - 1] = static_cast<uint8_t>((crc >> 24) & 0xFF);
  DeviceConfig out;
  uint32_t generation = 0;
  TEST_ASSERT_TRUE(tth::config::decodeRecord(buffer, n, out, generation) ==
                   RecordStatus::InvalidField);
}

static void an_incomplete_config_is_never_encoded() {
  DeviceConfig c = complete();
  c.presentMask = 0x0F;  // token missing
  uint8_t buffer[tth::config::kRecordMaxBytes];
  TEST_ASSERT_EQUAL_UINT32(0, tth::config::encodeRecord(c, 1, buffer, sizeof(buffer)));
}

static void field_names_round_trip_and_secrets_are_marked() {
  for (uint8_t i = 0; i < tth::config::kFieldCount; ++i) {
    const Field f = static_cast<Field>(i);
    const char* name = tth::config::toString(f);
    Field parsed = Field::Ssid;
    TEST_ASSERT_TRUE(tth::config::fieldFromName(name, strlen(name), parsed));
    TEST_ASSERT_TRUE(parsed == f);
  }
  Field unused = Field::Ssid;
  TEST_ASSERT_FALSE(tth::config::fieldFromName("password", 8, unused));
  TEST_ASSERT_TRUE(tth::config::isSecret(Field::Ssid));
  TEST_ASSERT_TRUE(tth::config::isSecret(Field::Password));
  TEST_ASSERT_TRUE(tth::config::isSecret(Field::DeviceToken));
  TEST_ASSERT_FALSE(tth::config::isSecret(Field::GatewayUrl));
  TEST_ASSERT_FALSE(tth::config::isSecret(Field::DeviceId));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(ssid_lengths_and_characters);
  RUN_TEST(password_rules);
  RUN_TEST(device_id_rules);
  RUN_TEST(token_rules);
  RUN_TEST(url_accepts_wss_and_reports_its_parts);
  RUN_TEST(url_refuses_every_scheme_but_wss);
  RUN_TEST(url_refuses_userinfo_query_fragment_and_whitespace);
  RUN_TEST(url_host_and_port_rules);
  RUN_TEST(url_path_and_length_rules);
  RUN_TEST(a_rejected_value_leaves_the_field_exactly_as_it_was);
  RUN_TEST(completeness_names_the_first_missing_field);
  RUN_TEST(clear_zeroes_every_byte);
  RUN_TEST(crc32_matches_the_standard_check_value);
  RUN_TEST(a_record_round_trips);
  RUN_TEST(the_largest_record_fits_the_maximum);
  RUN_TEST(corrupted_records_are_rejected);
  RUN_TEST(a_valid_checksum_does_not_bypass_field_validation);
  RUN_TEST(an_incomplete_config_is_never_encoded);
  RUN_TEST(field_names_round_trip_and_secrets_are_marked);
  return UNITY_END();
}
