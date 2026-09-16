#include "tth/TrustAnchor.h"

#include <string.h>

namespace tth {
namespace trust {

namespace {

const char* const kBegin = "-----BEGIN CERTIFICATE-----";
const char* const kEnd = "-----END CERTIFICATE-----";

bool lineIs(const char* line, size_t length, const char* marker) {
  return strlen(marker) == length && memcmp(line, marker, length) == 0;
}

bool contains(const char* text, size_t length, const char* needle) {
  const size_t n = strlen(needle);
  if (n > length) return false;
  for (size_t i = 0; i + n <= length; ++i) {
    if (memcmp(text + i, needle, n) == 0) return true;
  }
  return false;
}

bool isBase64(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
         c == '+' || c == '/' || c == '=';
}

}  // namespace

const char* toString(PemCheck check) {
  switch (check) {
    case PemCheck::Ok:
      return "ok";
    case PemCheck::Empty:
      return "empty";
    case PemCheck::TooLong:
      return "larger than 4096 bytes";
    case PemCheck::BadCharacter:
      return "invalid character";
    case PemCheck::NotCertificate:
      return "not a PEM certificate bundle";
    case PemCheck::PrivateKey:
      return "contains a PRIVATE KEY - provision the CA certificate, never a key";
    case PemCheck::TooManyCertificates:
      return "more than 3 certificates";
    case PemCheck::BadBase64:
      return "invalid base64 line";
    case PemCheck::Unterminated:
      return "certificate not terminated";
    case PemCheck::NoCertificate:
      return "no certificate";
  }
  return "invalid";
}

PemCheck checkPemBundle(const char* pem, size_t length, uint8_t* certificates) {
  if (certificates != nullptr) *certificates = 0;
  if (pem == nullptr || length == 0) return PemCheck::Empty;
  if (length > kMaxPemBytes) return PemCheck::TooLong;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(pem[i]);
    if (c == '\n' || c == '\r') continue;
    if (c < 0x20 || c >= 0x7F) return PemCheck::BadCharacter;
  }
  if (contains(pem, length, "PRIVATE KEY")) return PemCheck::PrivateKey;

  bool inside = false;
  uint8_t count = 0;
  size_t bodyLines = 0;
  size_t pos = 0;
  while (pos < length) {
    size_t end = pos;
    while (end < length && pem[end] != '\n') ++end;
    size_t lineEnd = end;
    if (lineEnd > pos && pem[lineEnd - 1] == '\r') --lineEnd;
    const char* line = pem + pos;
    const size_t n = lineEnd - pos;
    pos = end + 1;

    if (n == 0) {
      if (inside) return PemCheck::BadBase64;
      continue;
    }
    if (!inside) {
      if (!lineIs(line, n, kBegin)) return PemCheck::NotCertificate;
      if (count == kMaxCertificates) return PemCheck::TooManyCertificates;
      ++count;
      inside = true;
      bodyLines = 0;
      continue;
    }
    if (lineIs(line, n, kEnd)) {
      if (bodyLines == 0) return PemCheck::BadBase64;
      inside = false;
      continue;
    }
    if (n >= 5 && memcmp(line, "-----", 5) == 0) return PemCheck::NotCertificate;
    if (n > kMaxPemLineBytes) return PemCheck::BadBase64;
    for (size_t i = 0; i < n; ++i) {
      if (!isBase64(line[i])) return PemCheck::BadBase64;
    }
    ++bodyLines;
  }
  if (inside) return PemCheck::Unterminated;
  if (count == 0) return PemCheck::NoCertificate;
  if (certificates != nullptr) *certificates = count;
  return PemCheck::Ok;
}

}  // namespace trust

// --- store -------------------------------------------------------------------------

const char* const TrustAnchorStore::kSlotKeyA = "caA";
const char* const TrustAnchorStore::kSlotKeyB = "caB";

namespace {

const uint8_t kMagic[4] = {'T', 'T', 'H', 'A'};
const uint8_t kVersion = 1;
const size_t kPemOffset = 11;
const char kEmpty[1] = {'\0'};

void putU32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFFu);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
  out[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
  out[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

uint32_t getU32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
         (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

}  // namespace

TrustAnchorStore::TrustAnchorStore(IConfigStorage& storage)
    : _storage(storage),
      _active(nullptr),
      _scratchA(nullptr),
      _scratchB(nullptr),
      _hasRecord(false),
      _length(0),
      _certificates(0),
      _generation(0),
      _slot(0) {}

void TrustAnchorStore::attachBuffers(char* active, uint8_t* scratchA,
                                     uint8_t* scratchB) {
  _active = active;
  _scratchA = scratchA;
  _scratchB = scratchB;
  if (_active != nullptr) _active[0] = '\0';
}

bool TrustAnchorStore::hasBuffers() const {
  return _active != nullptr && _scratchA != nullptr && _scratchB != nullptr;
}

const char* TrustAnchorStore::pem() const {
  return (_active != nullptr && _length > 0) ? _active : kEmpty;
}

void TrustAnchorStore::forget() {
  _hasRecord = false;
  _length = 0;
  _certificates = 0;
  _generation = 0;
  _slot = 0;
  if (_active != nullptr) _active[0] = '\0';
}

config::RecordStatus TrustAnchorStore::decode(const uint8_t* data, size_t length,
                                              uint32_t& generation, bool copy) {
  generation = 0;
  if (length == 0) return config::RecordStatus::Absent;
  if (length < kRecordOverhead) return config::RecordStatus::TooShort;
  if (length > kScratchBytes) return config::RecordStatus::BadLength;
  if (memcmp(data, kMagic, 4) != 0) return config::RecordStatus::BadMagic;
  if (data[4] != kVersion) return config::RecordStatus::BadVersion;
  const size_t body = length - 4;
  if (config::crc32(data, body) != getU32(data + body)) return config::RecordStatus::BadCrc;
  const size_t pemLength =
      static_cast<size_t>(data[9]) | (static_cast<size_t>(data[10]) << 8);
  if (pemLength + kRecordOverhead != length) return config::RecordStatus::BadLength;
  uint8_t certificates = 0;
  if (pemLength > 0 &&
      trust::checkPemBundle(reinterpret_cast<const char*>(data + kPemOffset), pemLength,
                            &certificates) != trust::PemCheck::Ok) {
    return config::RecordStatus::InvalidField;
  }
  generation = getU32(data + 5);
  if (copy) {
    memcpy(_active, data + kPemOffset, pemLength);
    _active[pemLength] = '\0';
    _length = pemLength;
    _certificates = certificates;
  }
  return config::RecordStatus::Ok;
}

config::RecordStatus TrustAnchorStore::readSlot(const char* key, uint32_t& generation,
                                                bool copy) {
  generation = 0;
  const size_t n = _storage.read(key, _scratchA, kScratchBytes);
  if (n == 0) return config::RecordStatus::Absent;
  if (n > kScratchBytes) return config::RecordStatus::BadLength;
  return decode(_scratchA, n, generation, copy);
}

TrustLoadReport TrustAnchorStore::load() {
  TrustLoadReport report;
  report.a = config::RecordStatus::Absent;
  report.b = config::RecordStatus::Absent;
  report.generationA = 0;
  report.generationB = 0;
  forget();
  if (!hasBuffers()) {
    report.chosen = 0;
    report.generation = 0;
    return report;
  }

  report.a = readSlot(kSlotKeyA, report.generationA, false);
  report.b = readSlot(kSlotKeyB, report.generationB, false);
  const bool aOk = report.a == config::RecordStatus::Ok;
  const bool bOk = report.b == config::RecordStatus::Ok;
  char chosen = 0;
  if (aOk && (!bOk || report.generationA >= report.generationB)) {
    chosen = 'A';
  } else if (bOk) {
    chosen = 'B';
  }
  if (chosen != 0) {
    uint32_t generation = 0;
    if (readSlot(chosen == 'A' ? kSlotKeyA : kSlotKeyB, generation, true) ==
        config::RecordStatus::Ok) {
      _hasRecord = true;
      _generation = generation;
      _slot = chosen;
    } else {
      forget();
    }
  }
  report.chosen = _slot;
  report.generation = _generation;
  return report;
}

CommitResult TrustAnchorStore::commit(const char* pem, size_t length,
                                      trust::PemCheck* why) {
  uint8_t certificates = 0;
  const trust::PemCheck check = trust::checkPemBundle(pem, length, &certificates);
  if (why != nullptr) *why = check;
  if (check != trust::PemCheck::Ok) return CommitResult::Incomplete;
  return write(pem, length, certificates);
}

CommitResult TrustAnchorStore::clear() { return write(kEmpty, 0, 0); }

CommitResult TrustAnchorStore::write(const char* pem, size_t length,
                                     uint8_t certificates) {
  if (!hasBuffers() || length > trust::kMaxPemBytes) return CommitResult::WriteFailed;
  if (_hasRecord && _generation == 0xFFFFFFFFu) return CommitResult::GenerationExhausted;

  const uint32_t generation = _hasRecord ? _generation + 1u : 1u;
  const char targetSlot = (_slot == 'A') ? 'B' : 'A';
  const char* key = (targetSlot == 'A') ? kSlotKeyA : kSlotKeyB;

  memcpy(_scratchA, kMagic, 4);
  _scratchA[4] = kVersion;
  putU32(_scratchA + 5, generation);
  _scratchA[9] = static_cast<uint8_t>(length & 0xFFu);
  _scratchA[10] = static_cast<uint8_t>((length >> 8) & 0xFFu);
  if (length > 0) memcpy(_scratchA + kPemOffset, pem, length);
  const size_t body = kPemOffset + length;
  putU32(_scratchA + body, config::crc32(_scratchA, body));
  const size_t total = body + 4;

  if (!_storage.write(key, _scratchA, total)) return CommitResult::WriteFailed;

  const size_t back = _storage.read(key, _scratchB, kScratchBytes);
  bool verified = back == total && memcmp(_scratchB, _scratchA, total) == 0;
  if (verified) {
    uint32_t decodedGeneration = 0;
    verified = decode(_scratchB, back, decodedGeneration, false) == config::RecordStatus::Ok &&
               decodedGeneration == generation;
  }
  if (!verified) return CommitResult::VerifyFailed;

  if (length > 0) memcpy(_active, pem, length);
  _active[length] = '\0';
  _length = length;
  _certificates = certificates;
  _generation = generation;
  _slot = targetSlot;
  _hasRecord = true;
  return CommitResult::Ok;
}

}  // namespace tth
