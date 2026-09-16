// Host-side tests for the WebSocket client codec: SHA-1/base64 for the
// handshake, the upgrade request, the strict 101 check (and the HTTP status of
// a refusal), masked client frames, and the bounded server-frame decoder.

#include <string.h>

#include <string>
#include <vector>

#include <unity.h>

#include "tth/WebSocketCodec.h"

using tth::ws::DecodeError;
using tth::ws::DecodeStatus;
using tth::ws::FrameDecoder;
using tth::ws::HandshakeOutcome;
using tth::ws::HandshakeResult;
using tth::ws::Opcode;

void setUp() {}
void tearDown() {}

namespace {

const char* const kKey = "dGhlIHNhbXBsZSBub25jZQ==";
const char* const kAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
const char* const kToken = "TOKENSENTINELabcdefghijklmnopqrstuvwxyz0123";

std::string hex(const uint8_t* data, size_t length) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < length; ++i) {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 0x0F];
  }
  return out;
}

std::string sha1Hex(const std::string& s) {
  uint8_t digest[tth::ws::kSha1Bytes];
  tth::ws::sha1(reinterpret_cast<const uint8_t*>(s.data()), s.size(), digest);
  return hex(digest, sizeof(digest));
}

std::string b64(const std::string& s) {
  char out[64];
  const size_t n = tth::ws::base64Encode(reinterpret_cast<const uint8_t*>(s.data()),
                                         s.size(), out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT32(strlen(out), n);
  return out;
}

tth::ws::UpgradeRequest request() {
  tth::ws::UpgradeRequest r;
  r.host = "192.168.1.50";
  r.port = 8443;
  r.path = "/v1/ws";
  r.deviceId = "core2-01";
  r.token = kToken;
  r.key = kKey;
  r.userAgent = "tth-core2-6.2";
  return r;
}

HandshakeResult parse(const std::string& response) {
  return tth::ws::parseUpgradeResponse(response.data(), response.size(), kAccept,
                                       tth::ws::kSubprotocol);
}

std::string good101() {
  return "HTTP/1.1 101 Switching Protocols\r\n"
         "Upgrade: websocket\r\n"
         "Connection: Upgrade\r\n"
         "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
         "Sec-WebSocket-Protocol: tth.v1\r\n"
         "\r\n";
}

std::vector<uint8_t> bytesOf(const std::string& s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

// An unmasked server frame.
std::vector<uint8_t> serverFrame(uint8_t first, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> f;
  f.push_back(first);
  if (payload.size() < 126) {
    f.push_back(static_cast<uint8_t>(payload.size()));
  } else {
    f.push_back(126);
    f.push_back(static_cast<uint8_t>(payload.size() >> 8));
    f.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
  }
  f.insert(f.end(), payload.begin(), payload.end());
  return f;
}

DecodeError decodeError(const std::vector<uint8_t>& frame, size_t capacity = 64) {
  std::vector<uint8_t> buffer(capacity);
  FrameDecoder d(buffer.data(), buffer.size());
  size_t used = 0;
  const DecodeStatus status = d.feed(frame.data(), frame.size(), used);
  TEST_ASSERT_TRUE(status == DecodeStatus::Error);
  return d.error();
}

}  // namespace

// --- hashing --------------------------------------------------------------------------

static void sha1_matches_the_standard_vectors() {
  TEST_ASSERT_EQUAL_STRING("da39a3ee5e6b4b0d3255bfef95601890afd80709", sha1Hex("").c_str());
  TEST_ASSERT_EQUAL_STRING("a9993e364706816aba3e25717850c26c9cd0d89d", sha1Hex("abc").c_str());
  TEST_ASSERT_EQUAL_STRING(
      "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
      sha1Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").c_str());
  TEST_ASSERT_EQUAL_STRING("34aa973cd4c4daa4f61eeb2bdbad27316534016f",
                           sha1Hex(std::string(1000000, 'a')).c_str());
  // Lengths around the padding boundary all hash, and differently.
  const std::string h55 = sha1Hex(std::string(55, 'x'));
  const std::string h56 = sha1Hex(std::string(56, 'x'));
  const std::string h64 = sha1Hex(std::string(64, 'x'));
  TEST_ASSERT_TRUE(h55 != h56 && h56 != h64 && h55 != h64);
}

static void base64_matches_rfc_4648() {
  TEST_ASSERT_EQUAL_STRING("", b64("").c_str());
  TEST_ASSERT_EQUAL_STRING("Zg==", b64("f").c_str());
  TEST_ASSERT_EQUAL_STRING("Zm8=", b64("fo").c_str());
  TEST_ASSERT_EQUAL_STRING("Zm9v", b64("foo").c_str());
  TEST_ASSERT_EQUAL_STRING("Zm9vYg==", b64("foob").c_str());
  TEST_ASSERT_EQUAL_STRING("Zm9vYmFy", b64("foobar").c_str());
  char small[4];
  TEST_ASSERT_EQUAL_UINT32(0, tth::ws::base64Encode(reinterpret_cast<const uint8_t*>("foo"),
                                                    3, small, sizeof(small)));
}

static void the_accept_key_matches_rfc_6455() {
  char accept[tth::ws::kAcceptBytes + 1];
  TEST_ASSERT_TRUE(tth::ws::computeAccept(kKey, accept));
  TEST_ASSERT_EQUAL_STRING(kAccept, accept);

  const uint8_t random16[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  char key[tth::ws::kKeyBase64Bytes + 1];
  TEST_ASSERT_TRUE(tth::ws::makeKey(random16, key));
  TEST_ASSERT_EQUAL_STRING("AAECAwQFBgcICQoLDA0ODw==", key);
}

// --- request -----------------------------------------------------------------------------

static void the_upgrade_request_is_exact() {
  char out[tth::ws::kMaxRequestBytes];
  const size_t n = tth::ws::buildUpgradeRequest(request(), out, sizeof(out));
  const std::string expected = std::string("GET /v1/ws HTTP/1.1\r\n") +
                               "Host: 192.168.1.50:8443\r\n"
                               "Upgrade: websocket\r\n"
                               "Connection: Upgrade\r\n"
                               "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                               "Sec-WebSocket-Version: 13\r\n"
                               "Sec-WebSocket-Protocol: tth.v1\r\n"
                               "Authorization: Bearer " + kToken + "\r\n"
                               "X-TTH-Device: core2-01\r\n"
                               "User-Agent: tth-core2-6.2\r\n"
                               "\r\n";
  TEST_ASSERT_EQUAL_UINT32(expected.size(), n);
  TEST_ASSERT_EQUAL_STRING(expected.c_str(), out);

  tth::ws::UpgradeRequest r = request();
  r.port = 443;
  r.host = "gw.example.test";
  tth::ws::buildUpgradeRequest(r, out, sizeof(out));
  TEST_ASSERT_NOT_NULL(strstr(out, "Host: gw.example.test\r\n"));
}

static void header_injection_and_empty_values_are_refused() {
  char out[tth::ws::kMaxRequestBytes];
  tth::ws::UpgradeRequest r = request();
  r.host = "evil\r\nX-Injected: 1";
  TEST_ASSERT_EQUAL_UINT32(0, tth::ws::buildUpgradeRequest(r, out, sizeof(out)));
  r = request();
  r.token = "abc\ndef";
  TEST_ASSERT_EQUAL_UINT32(0, tth::ws::buildUpgradeRequest(r, out, sizeof(out)));
  r = request();
  r.deviceId = "";
  TEST_ASSERT_EQUAL_UINT32(0, tth::ws::buildUpgradeRequest(r, out, sizeof(out)));
  r = request();
  r.path = "v1/ws";
  TEST_ASSERT_EQUAL_UINT32(0, tth::ws::buildUpgradeRequest(r, out, sizeof(out)));
  r = request();
  r.port = 0;
  TEST_ASSERT_EQUAL_UINT32(0, tth::ws::buildUpgradeRequest(r, out, sizeof(out)));
}

// A request that does not fit must not leave part of the token in the buffer.
static void a_truncated_request_leaves_no_token_behind() {
  char out[200];
  memset(out, 'x', sizeof(out));
  TEST_ASSERT_EQUAL_UINT32(0, tth::ws::buildUpgradeRequest(request(), out, sizeof(out)));
  for (size_t i = 0; i < sizeof(out); ++i) TEST_ASSERT_EQUAL_INT8(0, out[i]);
}

// --- response ----------------------------------------------------------------------------

static void a_correct_101_is_accepted() {
  const std::string response = good101();
  const HandshakeResult r = parse(response);
  TEST_ASSERT_TRUE(r.outcome == HandshakeOutcome::Accepted);
  TEST_ASSERT_EQUAL_UINT16(101, r.status);
  TEST_ASSERT_EQUAL_UINT32(response.size(), r.headBytes);
}

// Deno answers with lower-case names; Connection may be a list.
static void header_names_are_case_insensitive() {
  const std::string deno = "HTTP/1.1 101 Switching Protocols\r\n"
                           "upgrade: websocket\r\n"
                           "connection: keep-alive, Upgrade\r\n"
                           "sec-websocket-accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                           "sec-websocket-protocol: tth.v1\r\n"
                           "date: Mon, 14 Sep 2026 10:00:00 GMT\r\n"
                           "\r\n";
  TEST_ASSERT_TRUE(parse(deno).outcome == HandshakeOutcome::Accepted);
}

// The reason this codec exists: a refusal's HTTP status reaches the device.
static void a_refusal_reports_its_http_status() {
  const std::string head = "HTTP/1.1 401 Unauthorized\r\n"
                           "content-type: text/plain\r\n"
                           "content-length: 12\r\n"
                           "\r\n";
  const HandshakeResult r = parse(head + "unauthorized");
  TEST_ASSERT_TRUE(r.outcome == HandshakeOutcome::HttpStatus);
  TEST_ASSERT_EQUAL_UINT16(401, r.status);
  TEST_ASSERT_EQUAL_UINT32(head.size(), r.headBytes);
  TEST_ASSERT_EQUAL_UINT16(429, parse("HTTP/1.1 429 Too Many Requests\r\n\r\n").status);
  TEST_ASSERT_EQUAL_UINT16(503, parse("HTTP/1.1 503 Service Unavailable\r\n\r\n").status);
}

static void an_unfinished_or_endless_head_is_not_accepted() {
  const std::string response = good101();
  TEST_ASSERT_TRUE(parse(response.substr(0, response.size() - 2)).outcome ==
                   HandshakeOutcome::Incomplete);
  TEST_ASSERT_TRUE(parse("").outcome == HandshakeOutcome::Incomplete);
  const std::string endless = "HTTP/1.1 101 OK\r\nX-Pad: " + std::string(1100, 'a');
  TEST_ASSERT_TRUE(parse(endless).outcome == HandshakeOutcome::TooLong);
  TEST_ASSERT_TRUE(parse(endless + "\r\n\r\n").outcome == HandshakeOutcome::TooLong);
}

static void a_101_without_the_right_headers_is_refused() {
  std::string r = good101();
  std::string noUpgrade = r;
  noUpgrade.erase(noUpgrade.find("Upgrade: websocket\r\n"), strlen("Upgrade: websocket\r\n"));
  TEST_ASSERT_TRUE(parse(noUpgrade).outcome == HandshakeOutcome::BadUpgrade);

  std::string wrongAccept = r;
  wrongAccept.replace(wrongAccept.find("s3pP"), 4, "AAAA");
  TEST_ASSERT_TRUE(parse(wrongAccept).outcome == HandshakeOutcome::BadAccept);

  std::string noAccept = r;
  const std::string acceptLine = std::string("Sec-WebSocket-Accept: ") + kAccept + "\r\n";
  noAccept.erase(noAccept.find(acceptLine), acceptLine.size());
  TEST_ASSERT_TRUE(parse(noAccept).outcome == HandshakeOutcome::BadAccept);

  std::string otherProtocol = r;
  otherProtocol.replace(otherProtocol.find("tth.v1"), 6, "tth.v2");
  TEST_ASSERT_TRUE(parse(otherProtocol).outcome == HandshakeOutcome::BadProtocol);

  std::string noProtocol = r;
  noProtocol.erase(noProtocol.find("Sec-WebSocket-Protocol: tth.v1\r\n"),
                   strlen("Sec-WebSocket-Protocol: tth.v1\r\n"));
  TEST_ASSERT_TRUE(parse(noProtocol).outcome == HandshakeOutcome::BadProtocol);
}

static void malformed_responses_are_refused() {
  TEST_ASSERT_TRUE(parse("HTTP/1.1 1a1 Bad\r\n\r\n").outcome == HandshakeOutcome::Malformed);
  TEST_ASSERT_TRUE(parse("SSH-2.0-OpenSSH_9.0\r\n\r\n").outcome == HandshakeOutcome::Malformed);
  TEST_ASSERT_TRUE(parse("HTTP/1.1 101x\r\n\r\n").outcome == HandshakeOutcome::Malformed);
  TEST_ASSERT_TRUE(parse("HTTP/1.1 101 OK\r\nno colon here\r\n\r\n").outcome ==
                   HandshakeOutcome::Malformed);
}

// --- client frames -------------------------------------------------------------------------

static void client_frames_are_final_and_masked() {
  const uint8_t mask[4] = {1, 2, 3, 4};
  uint8_t out[16];
  const size_t n = tth::ws::encodeClientFrame(
      Opcode::Text, reinterpret_cast<const uint8_t*>("hi"), 2, mask, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT32(8, n);
  const uint8_t expected[8] = {0x81, 0x82, 1, 2, 3, 4, 'h' ^ 1, 'i' ^ 2};
  TEST_ASSERT_EQUAL_MEMORY(expected, out, 8);

  std::vector<uint8_t> payload(200, 0x55);
  std::vector<uint8_t> big(256);
  const size_t m = tth::ws::encodeClientFrame(Opcode::Binary, payload.data(), payload.size(),
                                              mask, big.data(), big.size());
  TEST_ASSERT_EQUAL_UINT32(208, m);
  TEST_ASSERT_EQUAL_HEX8(0x82, big[0]);
  TEST_ASSERT_EQUAL_HEX8(0xFE, big[1]);
  TEST_ASSERT_EQUAL_HEX8(0x00, big[2]);
  TEST_ASSERT_EQUAL_HEX8(0xC8, big[3]);
  for (size_t i = 0; i < 200; ++i) {
    TEST_ASSERT_EQUAL_HEX8(0x55, static_cast<uint8_t>(big[8 + i] ^ mask[i % 4]));
  }
}

static void invalid_client_frames_are_refused() {
  const uint8_t mask[4] = {9, 9, 9, 9};
  std::vector<uint8_t> payload(126, 0);
  std::vector<uint8_t> out(70000);
  TEST_ASSERT_EQUAL_UINT32(0, tth::ws::encodeClientFrame(Opcode::Ping, payload.data(), 126,
                                                         mask, out.data(), out.size()));
  TEST_ASSERT_EQUAL_UINT32(0, tth::ws::encodeClientFrame(Opcode::Continuation, payload.data(),
                                                         1, mask, out.data(), out.size()));
  TEST_ASSERT_EQUAL_UINT32(0, tth::ws::encodeClientFrame(Opcode::Text, payload.data(), 126,
                                                         mask, out.data(), 100));
  std::vector<uint8_t> huge(65536, 0);
  TEST_ASSERT_EQUAL_UINT32(0, tth::ws::encodeClientFrame(Opcode::Binary, huge.data(),
                                                         huge.size(), mask, out.data(),
                                                         out.size()));
}

// --- server frames ---------------------------------------------------------------------------

static void a_frame_decodes_byte_by_byte() {
  uint8_t buffer[64];
  FrameDecoder d(buffer, sizeof(buffer));
  const std::vector<uint8_t> f = serverFrame(0x81, bytesOf("hello"));
  for (size_t i = 0; i < f.size(); ++i) {
    size_t used = 0;
    const DecodeStatus status = d.feed(&f[i], 1, used);
    TEST_ASSERT_EQUAL_UINT32(1, used);
    TEST_ASSERT_TRUE(status == (i + 1 < f.size() ? DecodeStatus::NeedMore : DecodeStatus::Frame));
  }
  TEST_ASSERT_TRUE(d.frame().opcode == Opcode::Text);
  TEST_ASSERT_EQUAL_UINT32(5, d.frame().length);
  TEST_ASSERT_EQUAL_MEMORY("hello", d.frame().payload, 5);
}

static void back_to_back_frames_come_out_one_at_a_time() {
  uint8_t buffer[64];
  FrameDecoder d(buffer, sizeof(buffer));
  std::vector<uint8_t> both = serverFrame(0x89, bytesOf("p"));
  const std::vector<uint8_t> text = serverFrame(0x81, bytesOf("{\"t\":\"pong\"}"));
  both.insert(both.end(), text.begin(), text.end());

  size_t used = 0;
  TEST_ASSERT_TRUE(d.feed(both.data(), both.size(), used) == DecodeStatus::Frame);
  TEST_ASSERT_EQUAL_UINT32(3, used);
  TEST_ASSERT_TRUE(d.frame().opcode == Opcode::Ping);
  size_t used2 = 0;
  TEST_ASSERT_TRUE(d.feed(both.data() + used, both.size() - used, used2) == DecodeStatus::Frame);
  TEST_ASSERT_TRUE(d.frame().opcode == Opcode::Text);
  TEST_ASSERT_EQUAL_UINT32(text.size(), used2);
}

static void extended_lengths_are_bounded_by_the_buffer() {
  const std::vector<uint8_t> f = serverFrame(0x82, std::vector<uint8_t>(300, 7));
  std::vector<uint8_t> big(2048);
  FrameDecoder d(big.data(), big.size());
  size_t used = 0;
  TEST_ASSERT_TRUE(d.feed(f.data(), f.size(), used) == DecodeStatus::Frame);
  TEST_ASSERT_EQUAL_UINT32(300, d.frame().length);
  TEST_ASSERT_TRUE(decodeError(f, 256) == DecodeError::TooLarge);
  const std::vector<uint8_t> sixtyFour = {0x82, 127, 0, 0, 0, 0, 0, 0, 1, 0};
  TEST_ASSERT_TRUE(decodeError(sixtyFour) == DecodeError::TooLarge);
}

static void protocol_violations_are_errors() {
  TEST_ASSERT_TRUE(decodeError({0x81, 0x81, 1, 2, 3, 4, 'x'}) == DecodeError::Masked);
  TEST_ASSERT_TRUE(decodeError({0xC1, 0x01, 'x'}) == DecodeError::ReservedBits);
  TEST_ASSERT_TRUE(decodeError({0x01, 0x01, 'x'}) == DecodeError::Fragmented);
  TEST_ASSERT_TRUE(decodeError({0x80, 0x01, 'x'}) == DecodeError::Fragmented);
  TEST_ASSERT_TRUE(decodeError({0x83, 0x01, 'x'}) == DecodeError::UnknownOpcode);
  TEST_ASSERT_TRUE(decodeError({0x89, 126, 0, 126}) == DecodeError::ControlTooLong);
}

static void an_error_is_sticky_until_reset() {
  uint8_t buffer[64];
  FrameDecoder d(buffer, sizeof(buffer));
  const uint8_t bad[] = {0x81, 0x81, 1, 2, 3, 4, 'x'};
  size_t used = 0;
  TEST_ASSERT_TRUE(d.feed(bad, sizeof(bad), used) == DecodeStatus::Error);
  const std::vector<uint8_t> good = serverFrame(0x81, bytesOf("ok"));
  TEST_ASSERT_TRUE(d.feed(good.data(), good.size(), used) == DecodeStatus::Error);
  TEST_ASSERT_EQUAL_UINT32(0, used);
  d.reset();
  TEST_ASSERT_TRUE(d.feed(good.data(), good.size(), used) == DecodeStatus::Frame);
}

static void close_frames_carry_their_code() {
  uint8_t buffer[64];
  FrameDecoder d(buffer, sizeof(buffer));
  const std::vector<uint8_t> close = serverFrame(0x88, {0x03, 0xE8, 'b', 'y', 'e'});
  size_t used = 0;
  TEST_ASSERT_TRUE(d.feed(close.data(), close.size(), used) == DecodeStatus::Frame);
  TEST_ASSERT_EQUAL_UINT16(1000, tth::ws::closeCode(d.frame()));
  const std::vector<uint8_t> bare = serverFrame(0x88, {});
  TEST_ASSERT_TRUE(d.feed(bare.data(), bare.size(), used) == DecodeStatus::Frame);
  TEST_ASSERT_EQUAL_UINT32(0, d.frame().length);
  TEST_ASSERT_EQUAL_UINT16(1005, tth::ws::closeCode(d.frame()));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(sha1_matches_the_standard_vectors);
  RUN_TEST(base64_matches_rfc_4648);
  RUN_TEST(the_accept_key_matches_rfc_6455);
  RUN_TEST(the_upgrade_request_is_exact);
  RUN_TEST(header_injection_and_empty_values_are_refused);
  RUN_TEST(a_truncated_request_leaves_no_token_behind);
  RUN_TEST(a_correct_101_is_accepted);
  RUN_TEST(header_names_are_case_insensitive);
  RUN_TEST(a_refusal_reports_its_http_status);
  RUN_TEST(an_unfinished_or_endless_head_is_not_accepted);
  RUN_TEST(a_101_without_the_right_headers_is_refused);
  RUN_TEST(malformed_responses_are_refused);
  RUN_TEST(client_frames_are_final_and_masked);
  RUN_TEST(invalid_client_frames_are_refused);
  RUN_TEST(a_frame_decodes_byte_by_byte);
  RUN_TEST(back_to_back_frames_come_out_one_at_a_time);
  RUN_TEST(extended_lengths_are_bounded_by_the_buffer);
  RUN_TEST(protocol_violations_are_errors);
  RUN_TEST(an_error_is_sticky_until_reset);
  RUN_TEST(close_frames_carry_their_code);
  return UNITY_END();
}
