#include "tth/DeviceConfig.h"

#include <string.h>

namespace tth {
namespace config {

namespace {

bool isAlnum(uint8_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
}

bool isBase64Url(uint8_t c) { return isAlnum(c) || c == '-' || c == '_'; }

bool isHexDigit(uint8_t c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

bool isDigit(uint8_t c) { return c >= '0' && c <= '9'; }

bool checkHostname(const char* host, size_t length) {
  if (length == 0 || length > kHostMaxBytes) return false;
  size_t labelStart = 0;
  for (size_t i = 0; i <= length; ++i) {
    if (i == length || host[i] == '.') {
      const size_t labelLength = i - labelStart;
      if (labelLength == 0 || labelLength > 63) return false;  // empty label: leading, trailing or double dot
      if (host[labelStart] == '-' || host[i - 1] == '-') return false;
      labelStart = i + 1;
      continue;
    }
    const uint8_t c = static_cast<uint8_t>(host[i]);
    if (!isAlnum(c) && c != '-') return false;
  }
  return true;
}

bool checkIpv4(const char* host, size_t length) {
  int parts = 0;
  size_t i = 0;
  while (i <= length) {
    size_t digits = 0;
    uint32_t value = 0;
    while (i < length && isDigit(static_cast<uint8_t>(host[i]))) {
      value = value * 10u + static_cast<uint32_t>(host[i] - '0');
      ++digits;
      ++i;
      if (digits > 3) return false;
    }
    if (digits == 0 || value > 255) return false;
    ++parts;
    if (i == length) break;
    if (host[i] != '.') return false;
    ++i;
    if (i == length) return false;  // trailing dot
  }
  return parts == 4;
}

}  // namespace

const char* toString(Field field) {
  switch (field) {
    case Field::Ssid:
      return "ssid";
    case Field::Password:
      return "pass";
    case Field::GatewayUrl:
      return "url";
    case Field::DeviceId:
      return "id";
    case Field::DeviceToken:
      return "token";
  }
  return "invalid";
}

bool fieldFromName(const char* name, size_t length, Field& out) {
  for (uint8_t i = 0; i < kFieldCount; ++i) {
    const Field f = static_cast<Field>(i);
    const char* candidate = toString(f);
    if (strlen(candidate) == length && memcmp(candidate, name, length) == 0) {
      out = f;
      return true;
    }
  }
  return false;
}

bool isSecret(Field field) {
  return field == Field::Ssid || field == Field::Password ||
         field == Field::DeviceToken;
}

const char* toString(Check check) {
  switch (check) {
    case Check::Ok:
      return "ok";
    case Check::Empty:
      return "empty";
    case Check::TooShort:
      return "too short";
    case Check::TooLong:
      return "too long";
    case Check::BadCharacter:
      return "invalid character";
    case Check::BadLength:
      return "wrong length";
    case Check::NotHex:
      return "64 characters but not hexadecimal";
    case Check::InsecureScheme:
      return "insecure scheme ws:// refused - only wss:// is allowed";
    case Check::UnsupportedScheme:
      return "unsupported scheme - only wss:// is allowed";
    case Check::UserInfoNotAllowed:
      return "user info (@) is not allowed";
    case Check::QueryNotAllowed:
      return "query or fragment is not allowed";
    case Check::BadHost:
      return "invalid host (DNS name or IPv4 address)";
    case Check::BadPort:
      return "invalid port (1-65535)";
    case Check::MissingPath:
      return "missing path (for example /v1/ws)";
    case Check::BadPath:
      return "invalid path";
  }
  return "invalid";
}

Check checkSsid(const uint8_t* bytes, size_t length) {
  if (bytes == nullptr || length == 0) return Check::Empty;
  if (length > kSsidMaxBytes) return Check::TooLong;
  for (size_t i = 0; i < length; ++i) {
    if (bytes[i] < 0x20 || bytes[i] == 0x7F) return Check::BadCharacter;
  }
  return Check::Ok;
}

Check checkPassword(const uint8_t* bytes, size_t length) {
  if (length == 0) return Check::Ok;  // open network
  if (bytes == nullptr) return Check::Empty;
  if (length == kRawPskHexBytes) {
    for (size_t i = 0; i < length; ++i) {
      if (!isHexDigit(bytes[i])) return Check::NotHex;
    }
    return Check::Ok;
  }
  if (length > kRawPskHexBytes) return Check::TooLong;
  if (length < kPassphraseMinBytes) return Check::TooShort;
  for (size_t i = 0; i < length; ++i) {
    if (bytes[i] < 0x20 || bytes[i] > 0x7E) return Check::BadCharacter;
  }
  return Check::Ok;
}

Check checkGatewayUrl(const char* text, size_t length, GatewayUrlParts* parts) {
  if (text == nullptr || length == 0) return Check::Empty;
  if (length > kUrlMaxBytes) return Check::TooLong;
  for (size_t i = 0; i < length; ++i) {
    const uint8_t c = static_cast<uint8_t>(text[i]);
    if (c <= 0x20 || c >= 0x7F) return Check::BadCharacter;
  }

  size_t pos;
  if (length >= 6 && memcmp(text, "wss://", 6) == 0) {
    pos = 6;
  } else if (length >= 5 && memcmp(text, "ws://", 5) == 0) {
    return Check::InsecureScheme;
  } else {
    return Check::UnsupportedScheme;
  }

  for (size_t i = pos; i < length; ++i) {
    if (text[i] == '?' || text[i] == '#') return Check::QueryNotAllowed;
  }

  size_t slash = pos;
  while (slash < length && text[slash] != '/') ++slash;
  for (size_t i = pos; i < slash; ++i) {
    if (text[i] == '@') return Check::UserInfoNotAllowed;
  }
  if (slash == length) return Check::MissingPath;
  if (slash == pos) return Check::BadHost;
  if (text[pos] == '[') return Check::BadHost;  // IPv6 literals are not supported

  size_t colon = slash;
  for (size_t i = pos; i < slash; ++i) {
    if (text[i] == ':') {
      if (colon != slash) return Check::BadHost;  // more than one ':'
      colon = i;
    }
  }

  const char* host = text + pos;
  const size_t hostLength = colon - pos;
  if (hostLength == 0 || hostLength > kHostMaxBytes) return Check::BadHost;

  uint32_t port = 443;
  if (colon != slash) {
    const size_t digits = slash - colon - 1;
    if (digits == 0 || digits > 5) return Check::BadPort;
    port = 0;
    for (size_t i = colon + 1; i < slash; ++i) {
      if (!isDigit(static_cast<uint8_t>(text[i]))) return Check::BadPort;
      port = port * 10u + static_cast<uint32_t>(text[i] - '0');
    }
    if (port == 0 || port > 65535) return Check::BadPort;
  }

  bool digitsAndDots = true;
  for (size_t i = 0; i < hostLength; ++i) {
    const uint8_t c = static_cast<uint8_t>(host[i]);
    if (!isDigit(c) && c != '.') {
      digitsAndDots = false;
      break;
    }
  }
  const bool ipv4 = digitsAndDots;
  if (ipv4 ? !checkIpv4(host, hostLength) : !checkHostname(host, hostLength)) {
    return Check::BadHost;
  }

  const size_t pathLength = length - slash;
  if (pathLength > kPathMaxBytes) return Check::BadPath;
  for (size_t i = slash; i < length; ++i) {
    const uint8_t c = static_cast<uint8_t>(text[i]);
    if (!isAlnum(c) && c != '-' && c != '.' && c != '_' && c != '~' && c != '/') {
      return Check::BadPath;
    }
  }

  if (parts != nullptr) {
    memset(parts, 0, sizeof(*parts));
    memcpy(parts->host, host, hostLength);
    parts->port = static_cast<uint16_t>(port);
    memcpy(parts->path, text + slash, pathLength);
    parts->isIpv4 = ipv4;
  }
  return Check::Ok;
}

Check checkDeviceId(const char* text, size_t length) {
  if (text == nullptr || length == 0) return Check::Empty;
  if (length > kDeviceIdMaxBytes) return Check::TooLong;
  for (size_t i = 0; i < length; ++i) {
    if (!isBase64Url(static_cast<uint8_t>(text[i]))) return Check::BadCharacter;
  }
  return Check::Ok;
}

Check checkDeviceToken(const char* text, size_t length) {
  if (text == nullptr || length == 0) return Check::Empty;
  if (length != kTokenBytes) return Check::BadLength;
  for (size_t i = 0; i < length; ++i) {
    if (!isBase64Url(static_cast<uint8_t>(text[i]))) return Check::BadCharacter;
  }
  return Check::Ok;
}

Check checkField(Field field, const uint8_t* bytes, size_t length) {
  switch (field) {
    case Field::Ssid:
      return checkSsid(bytes, length);
    case Field::Password:
      return checkPassword(bytes, length);
    case Field::GatewayUrl:
      return checkGatewayUrl(reinterpret_cast<const char*>(bytes), length, nullptr);
    case Field::DeviceId:
      return checkDeviceId(reinterpret_cast<const char*>(bytes), length);
    case Field::DeviceToken:
      return checkDeviceToken(reinterpret_cast<const char*>(bytes), length);
  }
  return Check::Empty;
}

void clear(DeviceConfig& config) { memset(&config, 0, sizeof(config)); }

bool isPresent(const DeviceConfig& config, Field field) {
  return (config.presentMask & (1u << static_cast<uint8_t>(field))) != 0;
}

const uint8_t* fieldBytes(const DeviceConfig& config, Field field,
                          size_t& length) {
  switch (field) {
    case Field::Ssid:
      length = config.ssidLength;
      return config.ssid;
    case Field::Password:
      length = config.passwordLength;
      return config.password;
    case Field::GatewayUrl:
      length = config.gatewayUrlLength;
      return reinterpret_cast<const uint8_t*>(config.gatewayUrl);
    case Field::DeviceId:
      length = config.deviceIdLength;
      return reinterpret_cast<const uint8_t*>(config.deviceId);
    case Field::DeviceToken:
      length = config.deviceTokenLength;
      return reinterpret_cast<const uint8_t*>(config.deviceToken);
  }
  length = 0;
  return nullptr;
}

bool setField(DeviceConfig& config, Field field, const uint8_t* bytes,
              size_t length, Check* why) {
  const Check result = checkField(field, bytes, length);
  if (why != nullptr) *why = result;
  if (result != Check::Ok) return false;

  const uint8_t n = static_cast<uint8_t>(length);
  switch (field) {
    case Field::Ssid:
      memset(config.ssid, 0, sizeof(config.ssid));
      memcpy(config.ssid, bytes, length);
      config.ssidLength = n;
      break;
    case Field::Password:
      memset(config.password, 0, sizeof(config.password));
      if (length > 0) memcpy(config.password, bytes, length);
      config.passwordLength = n;
      break;
    case Field::GatewayUrl:
      memset(config.gatewayUrl, 0, sizeof(config.gatewayUrl));
      memcpy(config.gatewayUrl, bytes, length);
      config.gatewayUrlLength = n;
      break;
    case Field::DeviceId:
      memset(config.deviceId, 0, sizeof(config.deviceId));
      memcpy(config.deviceId, bytes, length);
      config.deviceIdLength = n;
      break;
    case Field::DeviceToken:
      memset(config.deviceToken, 0, sizeof(config.deviceToken));
      memcpy(config.deviceToken, bytes, length);
      config.deviceTokenLength = n;
      break;
  }
  config.presentMask =
      static_cast<uint8_t>(config.presentMask | (1u << static_cast<uint8_t>(field)));
  return true;
}

Check checkComplete(const DeviceConfig& config, Field* problem) {
  for (uint8_t i = 0; i < kFieldCount; ++i) {
    const Field f = static_cast<Field>(i);
    if (!isPresent(config, f)) {
      if (problem != nullptr) *problem = f;
      return Check::Empty;
    }
    size_t length = 0;
    const uint8_t* bytes = fieldBytes(config, f, length);
    const Check result = checkField(f, bytes, length);
    if (result != Check::Ok) {
      if (problem != nullptr) *problem = f;
      return result;
    }
  }
  return Check::Ok;
}

// --- record ----------------------------------------------------------------------

namespace {
const uint8_t kMagic[4] = {'T', 'T', 'H', 'C'};
const size_t kHeaderBytes = 4 + 1 + 4;
const size_t kCrcBytes = 4;

void putU32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFFu);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
  out[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
  out[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

uint32_t getU32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
         (static_cast<uint32_t>(in[2]) << 16) |
         (static_cast<uint32_t>(in[3]) << 24);
}
}  // namespace

uint32_t crc32(const uint8_t* data, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
  }
  return ~crc;
}

const char* toString(RecordStatus status) {
  switch (status) {
    case RecordStatus::Ok:
      return "valid";
    case RecordStatus::Absent:
      return "absent";
    case RecordStatus::TooShort:
      return "too short";
    case RecordStatus::BadLength:
      return "bad length";
    case RecordStatus::BadMagic:
      return "not a TTH record";
    case RecordStatus::BadVersion:
      return "unsupported version";
    case RecordStatus::BadCrc:
      return "checksum mismatch";
    case RecordStatus::InvalidField:
      return "invalid field";
  }
  return "invalid";
}

size_t encodeRecord(const DeviceConfig& config, uint32_t generation,
                    uint8_t* out, size_t capacity) {
  if (checkComplete(config, nullptr) != Check::Ok || out == nullptr) return 0;

  size_t need = kHeaderBytes + kCrcBytes;
  for (uint8_t i = 0; i < kFieldCount; ++i) {
    size_t length = 0;
    fieldBytes(config, static_cast<Field>(i), length);
    need += 1 + length;
  }
  if (capacity < need) return 0;

  memcpy(out, kMagic, 4);
  out[4] = kRecordVersion;
  putU32(out + 5, generation);
  size_t pos = kHeaderBytes;
  for (uint8_t i = 0; i < kFieldCount; ++i) {
    size_t length = 0;
    const uint8_t* bytes = fieldBytes(config, static_cast<Field>(i), length);
    out[pos++] = static_cast<uint8_t>(length);
    if (length > 0) memcpy(out + pos, bytes, length);
    pos += length;
  }
  putU32(out + pos, crc32(out, pos));
  return need;
}

RecordStatus decodeRecord(const uint8_t* data, size_t length,
                          DeviceConfig& out, uint32_t& generation) {
  clear(out);
  generation = 0;
  if (data == nullptr || length == 0) return RecordStatus::Absent;
  if (length < kHeaderBytes + kFieldCount + kCrcBytes) return RecordStatus::TooShort;
  if (length > kRecordMaxBytes) return RecordStatus::BadLength;
  if (memcmp(data, kMagic, 4) != 0) return RecordStatus::BadMagic;
  if (data[4] != kRecordVersion) return RecordStatus::BadVersion;

  const size_t body = length - kCrcBytes;
  if (crc32(data, body) != getU32(data + body)) return RecordStatus::BadCrc;

  size_t pos = kHeaderBytes;
  for (uint8_t i = 0; i < kFieldCount; ++i) {
    if (pos >= body) {
      clear(out);
      return RecordStatus::BadLength;
    }
    const size_t fieldLength = data[pos++];
    if (pos + fieldLength > body) {
      clear(out);
      return RecordStatus::BadLength;
    }
    // Re-validated on load: a record that passes the CRC still cannot put an
    // invalid value into use.
    if (!setField(out, static_cast<Field>(i), data + pos, fieldLength, nullptr)) {
      clear(out);
      return RecordStatus::InvalidField;
    }
    pos += fieldLength;
  }
  if (pos != body) {
    clear(out);
    return RecordStatus::BadLength;
  }
  generation = getU32(data + 5);
  return RecordStatus::Ok;
}

}  // namespace config
}  // namespace tth
