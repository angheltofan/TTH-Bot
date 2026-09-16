// Host-side tests for the device side of the tth.v1 wire protocol.
//
// The literal message strings below are the CROSS-LANGUAGE VECTORS: the
// gateway's Deno tests (gateway/tests/protocol_test.ts) use the same strings,
// so the C++ encoder and the TypeScript parser (and vice versa) are pinned to
// one byte-exact format.

#include <stdio.h>
#include <string.h>

#include <string>

#include <unity.h>

#include "tth/GatewayProtocol.h"

using tth::wire::AudioFrameView;
using tth::wire::ControlError;
using tth::wire::ControlMessage;
using tth::wire::ControlType;
using tth::wire::FrameError;

void setUp() {}
void tearDown() {}

namespace {

ControlError parse(const char* text, ControlMessage& out) {
  return tth::wire::parseControl(text, strlen(text), out);
}

}  // namespace

// --- binary header ------------------------------------------------------------

static void the_header_carries_a_uint32_little_endian_turn() {
  uint8_t frame[8 + 4] = {0};
  TEST_ASSERT_TRUE(
      tth::wire::writeAudioHeader(frame, tth::wire::kKindUserAudio, 0x12345678u));
  const uint8_t expected[8] = {0x01, 0x00, 0x00, 0x00, 0x78, 0x56, 0x34, 0x12};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, 8);

  AudioFrameView view;
  TEST_ASSERT_TRUE(tth::wire::parseAudioFrame(frame, sizeof(frame),
                                              tth::wire::kKindUserAudio,
                                              view) == FrameError::None);
  TEST_ASSERT_EQUAL_UINT32(0x12345678u, view.turn);
  TEST_ASSERT_EQUAL_UINT32(4, view.pcmBytes);
  TEST_ASSERT_EQUAL_PTR(frame + 8, view.pcm);
}

static void the_largest_turn_id_round_trips() {
  uint8_t frame[8 + 2] = {0};
  TEST_ASSERT_TRUE(tth::wire::writeAudioHeader(
      frame, tth::wire::kKindModelAudio, 0xFFFFFFFFu));
  AudioFrameView view;
  TEST_ASSERT_TRUE(tth::wire::parseAudioFrame(frame, sizeof(frame),
                                              tth::wire::kKindModelAudio,
                                              view) == FrameError::None);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, view.turn);
}

static void turn_zero_is_reserved_and_rejected() {
  uint8_t frame[10] = {0};
  TEST_ASSERT_FALSE(
      tth::wire::writeAudioHeader(frame, tth::wire::kKindUserAudio, 0));
  frame[0] = tth::wire::kKindUserAudio;  // turn bytes left at zero
  AudioFrameView view;
  TEST_ASSERT_TRUE(tth::wire::parseAudioFrame(frame, sizeof(frame),
                                              tth::wire::kKindUserAudio,
                                              view) == FrameError::ZeroTurn);
}

static void malformed_frames_are_rejected() {
  AudioFrameView view;
  uint8_t frame[8 + 1924] = {0};
  tth::wire::writeAudioHeader(frame, tth::wire::kKindModelAudio, 7);

  TEST_ASSERT_TRUE(tth::wire::parseAudioFrame(frame, 7,
                                              tth::wire::kKindModelAudio,
                                              view) == FrameError::TooShort);
  TEST_ASSERT_TRUE(tth::wire::parseAudioFrame(frame, 8,
                                              tth::wire::kKindModelAudio,
                                              view) == FrameError::EmptyPayload);
  TEST_ASSERT_TRUE(tth::wire::parseAudioFrame(frame, 8 + 3,
                                              tth::wire::kKindModelAudio,
                                              view) == FrameError::OddPayload);
  TEST_ASSERT_TRUE(tth::wire::parseAudioFrame(frame, 8 + 1920,
                                              tth::wire::kKindModelAudio,
                                              view) == FrameError::None);
  TEST_ASSERT_TRUE(tth::wire::parseAudioFrame(frame, 8 + 1922,
                                              tth::wire::kKindModelAudio,
                                              view) == FrameError::Oversize);
  // A model frame is not accepted where a user frame is expected.
  TEST_ASSERT_TRUE(tth::wire::parseAudioFrame(frame, 8 + 2,
                                              tth::wire::kKindUserAudio,
                                              view) == FrameError::BadKind);

  frame[1] = 1;
  TEST_ASSERT_TRUE(tth::wire::parseAudioFrame(frame, 8 + 2,
                                              tth::wire::kKindModelAudio,
                                              view) == FrameError::BadFlags);
  frame[1] = 0;
  frame[3] = 1;
  TEST_ASSERT_TRUE(tth::wire::parseAudioFrame(frame, 8 + 2,
                                              tth::wire::kKindModelAudio,
                                              view) == FrameError::BadReserved);
}

static void an_up_frame_is_at_most_640_bytes_of_pcm() {
  uint8_t frame[8 + 642] = {0};
  tth::wire::writeAudioHeader(frame, tth::wire::kKindUserAudio, 1);
  AudioFrameView view;
  TEST_ASSERT_TRUE(tth::wire::parseAudioFrame(frame, 8 + 640,
                                              tth::wire::kKindUserAudio,
                                              view) == FrameError::None);
  TEST_ASSERT_TRUE(tth::wire::parseAudioFrame(frame, 8 + 642,
                                              tth::wire::kKindUserAudio,
                                              view) == FrameError::Oversize);
}

// --- encoders: the cross-language vectors ------------------------------------------

static void device_messages_encode_byte_exactly() {
  char out[tth::wire::kMaxControlBytes + 1];

  TEST_ASSERT_TRUE(tth::wire::encodeHello(out, sizeof(out), "core2-6.0", 192000) > 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"t\":\"hello\",\"proto\":1,\"fw\":\"core2-6.0\",\"in\":\"s16le/16000/1\","
      "\"out\":\"s16le/24000/1\",\"maxDown\":1920,\"credit\":192000}",
      out);

  TEST_ASSERT_TRUE(tth::wire::encodeTurnStart(out, sizeof(out), 7) > 0);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"turn_start\",\"turn\":7}", out);

  TEST_ASSERT_TRUE(tth::wire::encodeTurnStart(out, sizeof(out), 4294967295u) > 0);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"turn_start\",\"turn\":4294967295}", out);

  TEST_ASSERT_TRUE(tth::wire::encodeTurnEnd(out, sizeof(out), 7, 12, 7680) > 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"t\":\"turn_end\",\"turn\":7,\"frames\":12,\"bytes\":7680}", out);

  TEST_ASSERT_TRUE(tth::wire::encodeCancel(out, sizeof(out), 7) > 0);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"cancel\",\"turn\":7}", out);

  TEST_ASSERT_TRUE(tth::wire::encodeCredit(out, sizeof(out), 3840) > 0);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"credit\",\"bytes\":3840}", out);

  TEST_ASSERT_TRUE(tth::wire::encodePing(out, sizeof(out), 123) > 0);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"ping\",\"ts\":123}", out);
}

static void encoders_refuse_invalid_arguments_and_small_buffers() {
  char out[tth::wire::kMaxControlBytes + 1];
  TEST_ASSERT_EQUAL_UINT32(0, tth::wire::encodeTurnStart(out, sizeof(out), 0));
  TEST_ASSERT_EQUAL_UINT32(0, tth::wire::encodeTurnEnd(out, sizeof(out), 0, 1, 2));
  TEST_ASSERT_EQUAL_UINT32(0, tth::wire::encodeCancel(out, sizeof(out), 0));
  TEST_ASSERT_EQUAL_UINT32(0, tth::wire::encodeHello(out, sizeof(out), "bad fw", 1));
  TEST_ASSERT_EQUAL_UINT32(0, tth::wire::encodeHello(out, sizeof(out), "a\"b", 1));
  TEST_ASSERT_EQUAL_UINT32(0, tth::wire::encodeHello(out, sizeof(out), "", 1));
  TEST_ASSERT_EQUAL_UINT32(
      0, tth::wire::encodeHello(out, sizeof(out), "0123456789012345678901234", 1));
  char tiny[8];
  TEST_ASSERT_EQUAL_UINT32(0, tth::wire::encodeTurnStart(tiny, sizeof(tiny), 7));
}

// --- parser: the gateway's vectors -------------------------------------------------

static void gateway_messages_parse() {
  ControlMessage m;

  TEST_ASSERT_TRUE(parse("{\"t\":\"ready\",\"session\":\"s-1\",\"activity\":\"a-1\","
                         "\"out\":\"s16le/24000/1\"}",
                         m) == ControlError::None);
  TEST_ASSERT_TRUE(m.type == ControlType::Ready);
  TEST_ASSERT_EQUAL_STRING("s-1", m.session);
  TEST_ASSERT_EQUAL_STRING("a-1", m.activity);
  TEST_ASSERT_EQUAL_STRING("s16le/24000/1", m.format);

  TEST_ASSERT_TRUE(parse("{\"t\":\"speech_start\",\"turn\":7,\"fmt\":\"s16le/24000/1\"}",
                         m) == ControlError::None);
  TEST_ASSERT_TRUE(m.type == ControlType::SpeechStart);
  TEST_ASSERT_EQUAL_UINT32(7, m.turn);
  TEST_ASSERT_EQUAL_STRING("s16le/24000/1", m.format);

  TEST_ASSERT_TRUE(parse("{\"t\":\"turn_complete\",\"turn\":7,\"frames\":30,"
                         "\"bytes\":57600}",
                         m) == ControlError::None);
  TEST_ASSERT_TRUE(m.type == ControlType::TurnComplete);
  TEST_ASSERT_EQUAL_UINT32(30, m.frames);
  TEST_ASSERT_EQUAL_UINT32(57600, m.bytes);

  TEST_ASSERT_TRUE(parse("{\"t\":\"interrupted\",\"turn\":7}", m) ==
                   ControlError::None);
  TEST_ASSERT_TRUE(m.type == ControlType::Interrupted);

  TEST_ASSERT_TRUE(parse("{\"t\":\"error\",\"code\":\"no_response\",\"retry\":true,"
                         "\"turn\":7}",
                         m) == ControlError::None);
  TEST_ASSERT_TRUE(m.type == ControlType::Error);
  TEST_ASSERT_EQUAL_STRING("no_response", m.code);
  TEST_ASSERT_TRUE(m.retry);
  TEST_ASSERT_TRUE(m.hasTurn);

  TEST_ASSERT_TRUE(parse("{\"t\":\"error\",\"code\":\"auth\",\"retry\":false}", m) ==
                   ControlError::None);
  TEST_ASSERT_FALSE(m.retry);
  TEST_ASSERT_FALSE(m.hasTurn);

  TEST_ASSERT_TRUE(parse("{\"t\":\"session_end\",\"reason\":\"max_age\"}", m) ==
                   ControlError::None);
  TEST_ASSERT_TRUE(m.type == ControlType::SessionEnd);
  TEST_ASSERT_EQUAL_STRING("max_age", m.reason);

  TEST_ASSERT_TRUE(parse("{\"t\":\"pong\",\"ts\":4294967295}", m) ==
                   ControlError::None);
  TEST_ASSERT_EQUAL_UINT32(4294967295u, m.ts);
}

static void whitespace_escapes_and_unknown_keys_are_tolerated() {
  ControlMessage m;
  TEST_ASSERT_TRUE(parse(" { \"t\" : \"error\" , \"code\" : \"a\\\"b\\\\c\\/d\" ,"
                         " \"retry\" : false , \"future\" : 12 } ",
                         m) == ControlError::None);
  TEST_ASSERT_EQUAL_STRING("a\"b\\c/d", m.code);
}

// A type this firmware does not know is ignored, not an error.
static void an_unknown_type_is_ignored() {
  ControlMessage m;
  TEST_ASSERT_TRUE(parse("{\"t\":\"future_thing\",\"x\":1}", m) ==
                   ControlError::None);
  TEST_ASSERT_TRUE(m.type == ControlType::Unknown);
}

static void malformed_messages_are_rejected() {
  ControlMessage m;
  TEST_ASSERT_TRUE(parse("", m) == ControlError::Malformed);
  TEST_ASSERT_TRUE(parse("[]", m) == ControlError::Malformed);
  TEST_ASSERT_TRUE(parse("{\"t\":\"pong\",\"ts\":1", m) == ControlError::Malformed);
  TEST_ASSERT_TRUE(parse("{\"t\":\"pong\",\"ts\":1} x", m) == ControlError::Malformed);
  TEST_ASSERT_TRUE(parse("{\"t\":\"pong\",\"ts\":{}}", m) == ControlError::Malformed);
  TEST_ASSERT_TRUE(parse("{\"t\":\"pong\",\"ts\":[1]}", m) == ControlError::Malformed);
  TEST_ASSERT_TRUE(parse("{\"t\":\"pong\",\"ts\":-1}", m) == ControlError::Malformed);
  TEST_ASSERT_TRUE(parse("{\"t\":\"pong\",\"ts\":1.5}", m) == ControlError::Malformed);
  TEST_ASSERT_TRUE(parse("{\"t\":\"pong\",\"ts\":01}", m) == ControlError::Malformed);
  TEST_ASSERT_TRUE(parse("{\"t\":\"pong\",\"ts\":4294967296}", m) ==
                   ControlError::Malformed);
  TEST_ASSERT_TRUE(parse("{\"t\":\"pong\",\"ts\":null}", m) == ControlError::Malformed);
  TEST_ASSERT_TRUE(parse("{\"t\":\"a\\u0041\"}", m) == ControlError::Malformed);
  TEST_ASSERT_TRUE(parse("{\"t\":\"pong\",\"ts\":1,\"ts\":2}", m) ==
                   ControlError::Malformed);
  TEST_ASSERT_TRUE(parse("{\"t\":\"x\ny\"}", m) == ControlError::Malformed);
}

static void missing_or_wrong_fields_are_rejected() {
  ControlMessage m;
  TEST_ASSERT_TRUE(parse("{\"turn\":7}", m) == ControlError::MissingType);
  TEST_ASSERT_TRUE(parse("{\"t\":7}", m) == ControlError::BadValue);
  TEST_ASSERT_TRUE(parse("{\"t\":\"speech_start\",\"turn\":0,\"fmt\":\"x\"}", m) ==
                   ControlError::BadValue);
  TEST_ASSERT_TRUE(parse("{\"t\":\"turn_complete\",\"turn\":7,\"bytes\":1}", m) ==
                   ControlError::MissingField);
  TEST_ASSERT_TRUE(parse("{\"t\":\"error\",\"code\":\"x\"}", m) ==
                   ControlError::MissingField);
  TEST_ASSERT_TRUE(parse("{\"t\":\"error\",\"code\":\"x\",\"retry\":\"yes\"}", m) ==
                   ControlError::BadValue);
  // Longer than the 31-character code field.
  TEST_ASSERT_TRUE(parse("{\"t\":\"error\",\"code\":\"abcdefghijklmnopqrstuvwxyz0123456789\","
                         "\"retry\":true}",
                         m) == ControlError::BadValue);
}

static void an_oversized_control_frame_is_rejected() {
  std::string big = "{\"t\":\"pong\",\"ts\":1,\"pad\":\"";
  while (big.size() < tth::wire::kMaxControlBytes + 10) big += "x";
  big += "\"}";
  ControlMessage m;
  TEST_ASSERT_TRUE(tth::wire::parseControl(big.c_str(), big.size(), m) ==
                   ControlError::TooLong);
}

// --- activity selection --------------------------------------------------------------

static const char* const kActivityA = "a95ffc7e-1406-4a19-ac3b-6c27d8516b70";

static void activity_messages_encode_byte_exactly() {
  char out[513];
  // Shared with gateway/tests/protocol_test.ts.
  TEST_ASSERT_TRUE(tth::wire::encodeHello(out, sizeof(out), "core2-6.4", 192000, kActivityA) > 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"t\":\"hello\",\"proto\":1,\"fw\":\"core2-6.4\",\"in\":\"s16le/16000/1\","
      "\"out\":\"s16le/24000/1\",\"maxDown\":1920,\"credit\":192000,"
      "\"activity\":\"a95ffc7e-1406-4a19-ac3b-6c27d8516b70\"}",
      out);
  // No saved selection: exactly the previous hello.
  TEST_ASSERT_TRUE(tth::wire::encodeHello(out, sizeof(out), "core2-6.4", 192000, "") > 0);
  TEST_ASSERT_NULL(strstr(out, "activity"));
  TEST_ASSERT_TRUE(tth::wire::encodeActivitySelect(out, sizeof(out), kActivityA) > 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"t\":\"activity_select\",\"activity\":\"a95ffc7e-1406-4a19-ac3b-6c27d8516b70\"}", out);
  // Anything but a canonical id is refused.
  TEST_ASSERT_EQUAL_UINT32(0, tth::wire::encodeActivitySelect(out, sizeof(out), "not-an-id"));
  TEST_ASSERT_EQUAL_UINT32(
      0, tth::wire::encodeActivitySelect(out, sizeof(out), "A95FFC7E-1406-4A19-AC3B-6C27D8516B70"));
  TEST_ASSERT_EQUAL_UINT32(0, tth::wire::encodeHello(out, sizeof(out), "core2-6.4", 1, "x\"y"));
  TEST_ASSERT_FALSE(tth::wire::isActivityId("a95ffc7e-1406-4a19-ac3b-6c27d8516b7"));
  TEST_ASSERT_FALSE(tth::wire::isActivityId("a95ffc7e-1406-4a19-ac3b-6c27d8516b700"));
  TEST_ASSERT_FALSE(tth::wire::isActivityId("a95ffc7e_1406-4a19-ac3b-6c27d8516b70"));
  TEST_ASSERT_FALSE(tth::wire::isActivityId(nullptr));
}

static void activity_list_items_parse_with_their_bounds() {
  ControlMessage m;
  // Shared with gateway/tests/protocol_test.ts (UTF-8 title, no escapes).
  const char* item =
      "{\"t\":\"activity_list\",\"index\":1,\"count\":4,"
      "\"activity\":\"a95ffc7e-1406-4a19-ac3b-6c27d8516b70\","
      "\"title\":\"Conversa\xC8\x9Bie liber\xC4\x83\",\"mode\":\"free_conversation\",\"current\":true}";
  TEST_ASSERT_TRUE(parse(item, m) == ControlError::None);
  TEST_ASSERT_TRUE(m.type == ControlType::ActivityList);
  TEST_ASSERT_EQUAL_UINT32(1, m.index);
  TEST_ASSERT_EQUAL_UINT32(4, m.count);
  TEST_ASSERT_TRUE(m.current);
  TEST_ASSERT_EQUAL_STRING(kActivityA, m.activity);
  TEST_ASSERT_EQUAL_STRING("Conversa\xC8\x9Bie liber\xC4\x83", m.title);
  TEST_ASSERT_EQUAL_STRING("free_conversation", m.mode);

  const auto variant = [](const char* index, const char* count, const char* id, const char* title,
                          const char* mode) {
    static char buffer[600];
    snprintf(buffer, sizeof(buffer),
             "{\"t\":\"activity_list\",\"index\":%s,\"count\":%s,\"activity\":\"%s\","
             "\"title\":\"%s\",\"mode\":\"%s\",\"current\":false}",
             index, count, id, title, mode);
    return buffer;
  };
  const std::string title48(48, 'x');
  const std::string title49(49, 'x');
  TEST_ASSERT_TRUE(parse(variant("7", "8", kActivityA, title48.c_str(), "push_to_talk"), m) ==
                   ControlError::None);
  TEST_ASSERT_TRUE(parse(variant("0", "0", kActivityA, "T", "push_to_talk"), m) ==
                   ControlError::BadValue);  // empty list
  TEST_ASSERT_TRUE(parse(variant("0", "9", kActivityA, "T", "push_to_talk"), m) ==
                   ControlError::BadValue);  // over 8
  TEST_ASSERT_TRUE(parse(variant("4", "4", kActivityA, "T", "push_to_talk"), m) ==
                   ControlError::BadValue);  // index out of range
  TEST_ASSERT_TRUE(parse(variant("0", "1", "not-an-id", "T", "push_to_talk"), m) ==
                   ControlError::BadValue);
  TEST_ASSERT_TRUE(parse(variant("0", "1", kActivityA, "", "push_to_talk"), m) ==
                   ControlError::BadValue);  // empty title
  TEST_ASSERT_TRUE(parse(variant("0", "1", kActivityA, title49.c_str(), "push_to_talk"), m) ==
                   ControlError::BadValue);  // title over 48 bytes
  TEST_ASSERT_TRUE(parse(variant("0", "1", kActivityA, "T", "telepathy"), m) ==
                   ControlError::BadValue);
  TEST_ASSERT_TRUE(parse("{\"t\":\"activity_list\",\"index\":0,\"count\":1,"
                         "\"activity\":\"a95ffc7e-1406-4a19-ac3b-6c27d8516b70\",\"title\":\"T\","
                         "\"mode\":\"push_to_talk\"}",
                         m) == ControlError::MissingField);  // no current
}

static void activity_selected_and_errors_parse() {
  ControlMessage m;
  TEST_ASSERT_TRUE(parse("{\"t\":\"activity_selected\",\"activity\":\"a95ffc7e-1406-4a19-ac3b-6c27d8516b70\"}",
                         m) == ControlError::None);
  TEST_ASSERT_TRUE(m.type == ControlType::ActivitySelected);
  TEST_ASSERT_EQUAL_STRING(kActivityA, m.activity);
  TEST_ASSERT_TRUE(parse("{\"t\":\"activity_selected\",\"activity\":\"a-1\"}", m) ==
                   ControlError::BadValue);

  TEST_ASSERT_TRUE(parse("{\"t\":\"activity_select_error\",\"code\":\"missing\","
                         "\"activity\":\"a95ffc7e-1406-4a19-ac3b-6c27d8516b70\"}",
                         m) == ControlError::None);
  TEST_ASSERT_TRUE(m.type == ControlType::ActivitySelectError);
  TEST_ASSERT_EQUAL_STRING("missing", m.code);
  TEST_ASSERT_TRUE(parse("{\"t\":\"activity_select_error\",\"code\":\"busy\"}", m) ==
                   ControlError::None);
  TEST_ASSERT_TRUE(parse("{\"t\":\"activity_select_error\",\"code\":\"busy\",\"activity\":\"x\"}",
                         m) == ControlError::BadValue);
  TEST_ASSERT_TRUE(parse("{\"t\":\"activity_select_error\"}", m) == ControlError::MissingField);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(the_header_carries_a_uint32_little_endian_turn);
  RUN_TEST(the_largest_turn_id_round_trips);
  RUN_TEST(turn_zero_is_reserved_and_rejected);
  RUN_TEST(malformed_frames_are_rejected);
  RUN_TEST(an_up_frame_is_at_most_640_bytes_of_pcm);
  RUN_TEST(device_messages_encode_byte_exactly);
  RUN_TEST(encoders_refuse_invalid_arguments_and_small_buffers);
  RUN_TEST(gateway_messages_parse);
  RUN_TEST(whitespace_escapes_and_unknown_keys_are_tolerated);
  RUN_TEST(an_unknown_type_is_ignored);
  RUN_TEST(malformed_messages_are_rejected);
  RUN_TEST(missing_or_wrong_fields_are_rejected);
  RUN_TEST(an_oversized_control_frame_is_rejected);
  RUN_TEST(activity_messages_encode_byte_exactly);
  RUN_TEST(activity_list_items_parse_with_their_bounds);
  RUN_TEST(activity_selected_and_errors_parse);
  return UNITY_END();
}
