#pragma once

#include <stddef.h>
#include <stdint.h>

// The device's persistent configuration: Wi-Fi credentials, the gateway URL,
// and the device identity (PHASE6_PLAN §4.1, Step 6.1).
//
// SECRETS: the SSID, the Wi-Fi password and the device token are never
// compiled into the firmware and never printed. Reports show only whether a
// field is configured plus safe metadata (a length; for the non-secret
// gateway URL and device id, the parsed host/port/path and the id).
//
// Every value is validated BEFORE it is accepted, and again when a stored
// record is loaded, so neither a malformed command nor a corrupted record can
// put an invalid value into use.
//
// Portable: no Arduino, no ESP-IDF.

namespace tth {
namespace config {

const size_t kSsidMaxBytes = 32;          // 802.11 limit
const size_t kPassphraseMinBytes = 8;     // WPA2 passphrase
const size_t kPassphraseMaxBytes = 63;
const size_t kRawPskHexBytes = 64;        // WPA2 raw PSK as 64 hex digits
const size_t kUrlMaxBytes = 200;
const size_t kHostMaxBytes = 253;
const size_t kPathMaxBytes = 180;
const size_t kDeviceIdMaxBytes = 32;      // same rule as the gateway registry
const size_t kTokenBytes = 43;            // 32 random bytes, base64url, no padding

enum class Field : uint8_t {
  Ssid = 0,
  Password,
  GatewayUrl,
  DeviceId,
  DeviceToken,
};

const uint8_t kFieldCount = 5;

// Command names: "ssid", "pass", "url", "id", "token".
const char* toString(Field field);
bool fieldFromName(const char* name, size_t length, Field& out);

// Values that must never appear in any output: SSID, password, token.
bool isSecret(Field field);

enum class Check : uint8_t {
  Ok = 0,
  Empty,
  TooShort,
  TooLong,
  BadCharacter,
  BadLength,
  NotHex,
  InsecureScheme,
  UnsupportedScheme,
  UserInfoNotAllowed,
  QueryNotAllowed,
  BadHost,
  BadPort,
  MissingPath,
  BadPath,
};

// Fixed text; never contains the value that was checked.
const char* toString(Check check);

// The non-secret parts of a gateway URL, for reports.
struct GatewayUrlParts {
  char host[kHostMaxBytes + 1];
  uint16_t port;
  char path[kPathMaxBytes + 1];
  bool isIpv4;
};

// 1-32 bytes; no control characters (spaces and UTF-8 are allowed).
Check checkSsid(const uint8_t* bytes, size_t length);

// Empty (open network), 8-63 printable ASCII, or exactly 64 hex digits.
Check checkPassword(const uint8_t* bytes, size_t length);

// ONLY wss:// — the plan's only approved scheme. Host is a DNS name or an
// IPv4 address; optional port 1-65535 (default 443); a path is required. No
// user info, query or fragment; no whitespace or control characters.
Check checkGatewayUrl(const char* text, size_t length, GatewayUrlParts* parts);

// 1-32 characters of [A-Za-z0-9_-].
Check checkDeviceId(const char* text, size_t length);

// Exactly 43 characters of [A-Za-z0-9_-].
Check checkDeviceToken(const char* text, size_t length);

Check checkField(Field field, const uint8_t* bytes, size_t length);

// Fixed-size storage; every array is zero-filled beyond its length.
struct DeviceConfig {
  uint8_t ssid[kSsidMaxBytes + 1];
  uint8_t ssidLength;
  uint8_t password[kRawPskHexBytes + 1];
  uint8_t passwordLength;
  char gatewayUrl[kUrlMaxBytes + 1];
  uint8_t gatewayUrlLength;
  char deviceId[kDeviceIdMaxBytes + 1];
  uint8_t deviceIdLength;
  char deviceToken[kTokenBytes + 1];
  uint8_t deviceTokenLength;
  uint8_t presentMask;  // one bit per Field
};

// Zero-fills everything, secrets included.
void clear(DeviceConfig& config);

bool isPresent(const DeviceConfig& config, Field field);

const uint8_t* fieldBytes(const DeviceConfig& config, Field field,
                          size_t& length);

// Validates first; on failure the config is left EXACTLY as it was and `why`
// says what was wrong.
bool setField(DeviceConfig& config, Field field, const uint8_t* bytes,
              size_t length, Check* why);

// Every field present and valid. On failure, `problem` is the first bad field.
Check checkComplete(const DeviceConfig& config, Field* problem);

// --- the stored record ---------------------------------------------------------
//
//   "TTHC" | version (1) | generation (u32 LE) |
//   5 x [ length (1) | bytes ] in Field order | CRC32 (u32 LE) of all before

const uint8_t kRecordVersion = 1;
const size_t kRecordMaxBytes = 4 + 1 + 4 + (1 + kSsidMaxBytes) +
                               (1 + kRawPskHexBytes) + (1 + kUrlMaxBytes) +
                               (1 + kDeviceIdMaxBytes) + (1 + kTokenBytes) + 4;

enum class RecordStatus : uint8_t {
  Ok = 0,
  Absent,
  TooShort,
  BadLength,
  BadMagic,
  BadVersion,
  BadCrc,
  InvalidField,
};

const char* toString(RecordStatus status);

// Complete configs only. Returns the record length, or 0.
size_t encodeRecord(const DeviceConfig& config, uint32_t generation,
                    uint8_t* out, size_t capacity);

// On anything but Ok, `out` is cleared.
RecordStatus decodeRecord(const uint8_t* data, size_t length,
                          DeviceConfig& out, uint32_t& generation);

// IEEE 802.3 CRC-32 (the zlib one): "123456789" -> 0xCBF43926.
uint32_t crc32(const uint8_t* data, size_t length);

}  // namespace config
}  // namespace tth
