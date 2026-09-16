#include "tth/Provisioning.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace tth {

namespace {

const size_t kOut = 224;

struct Cursor {
  const char* p;
  const char* end;
};

// A token ends at a single space or the end of the line. Exactly one space is
// consumed, so values keep leading spaces.
bool nextToken(Cursor& c, const char*& token, size_t& length) {
  if (c.p >= c.end) return false;
  const char* start = c.p;
  while (c.p < c.end && *c.p != ' ') ++c.p;
  token = start;
  length = static_cast<size_t>(c.p - start);
  if (c.p < c.end) ++c.p;
  return length > 0;
}

bool equals(const char* token, size_t length, const char* word) {
  return strlen(word) == length && memcmp(token, word, length) == 0;
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void emit(ILineSink& out, const char* format, ...)
    __attribute__((format(printf, 2, 3)));

void emit(ILineSink& out, const char* format, ...) {
  char line[kOut];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  out.line(line);
}

}  // namespace

void reportConfig(const char* label, const config::DeviceConfig& c,
                  bool present, ILineSink& out) {
  using config::Field;
  if (!present) {
    emit(out, "[config] %s: none", label);
    return;
  }

  if (config::isPresent(c, Field::Ssid)) {
    emit(out, "[config] %s ssid: configured (%u bytes)", label,
         static_cast<unsigned>(c.ssidLength));
  } else {
    emit(out, "[config] %s ssid: absent", label);
  }

  if (config::isPresent(c, Field::Password)) {
    emit(out, "[config] %s pass: configured%s", label,
         c.passwordLength == 0 ? " (open network)" : "");
  } else {
    emit(out, "[config] %s pass: absent", label);
  }

  config::GatewayUrlParts parts;
  if (config::isPresent(c, Field::GatewayUrl) &&
      config::checkGatewayUrl(c.gatewayUrl, c.gatewayUrlLength, &parts) ==
          config::Check::Ok) {
    emit(out, "[config] %s url: configured (wss host=%.64s%s port=%u path=%.48s%s)",
         label, parts.host, strlen(parts.host) > 64 ? "..." : "",
         static_cast<unsigned>(parts.port), parts.path,
         strlen(parts.path) > 48 ? "..." : "");
  } else {
    emit(out, "[config] %s url: absent", label);
  }

  if (config::isPresent(c, Field::DeviceId)) {
    emit(out, "[config] %s id: configured (%s)", label, c.deviceId);
  } else {
    emit(out, "[config] %s id: absent", label);
  }

  if (config::isPresent(c, Field::DeviceToken)) {
    emit(out, "[config] %s token: configured (%u chars)", label,
         static_cast<unsigned>(c.deviceTokenLength));
  } else {
    emit(out, "[config] %s token: absent", label);
  }

  Field problem = Field::Ssid;
  const config::Check complete = config::checkComplete(c, &problem);
  if (complete == config::Check::Ok) {
    emit(out, "[config] %s complete: yes", label);
  } else {
    emit(out, "[config] %s complete: no (%s: %s)", label,
         config::toString(problem), config::toString(complete));
  }
}

void reportTrustAnchor(const char* label, const TrustAnchorStore& store,
                       ICertificateCheck* check, ILineSink& out) {
  if (!store.hasAnchor()) {
    emit(out,
         "[config] %s ca: absent (the gateway connection is disabled until a CA "
         "is provisioned)",
         label);
    return;
  }
  char summary[96] = {0};
  if (check != nullptr) {
    check->check(store.pem(), store.length(), summary, sizeof(summary));
  }
  emit(out,
       "[config] %s ca: configured (%u certificate(s), %u bytes, generation %lu, "
       "slot %c)%s%.96s",
       label, static_cast<unsigned>(store.certificates()),
       static_cast<unsigned>(store.length()),
       static_cast<unsigned long>(store.generation()), store.activeSlot(),
       summary[0] != '\0' ? ": " : "", summary);
}

ProvisioningSession::ProvisioningSession(ConfigStore& store,
                                         IRandomSource& random)
    : _store(store),
      _random(random),
      _staging(false),
      _resetPending(false),
      _resetCode(0),
      _resetDeadlineMs(0),
      _trust(nullptr),
      _caStaging(nullptr),
      _caCapacity(0),
      _caLength(0),
      _caLines(0),
      _caActive(false),
      _certCheck(nullptr) {
  config::clear(_staged);
  memset(_decoded, 0, sizeof(_decoded));
}

void ProvisioningSession::attachTrustAnchors(TrustAnchorStore& store, char* staging,
                                             size_t capacity, ICertificateCheck* check) {
  if (staging == nullptr || capacity < trust::kMaxPemBytes + 1) return;
  _trust = &store;
  _caStaging = staging;
  _caCapacity = capacity;
  _certCheck = check;
  discardCa();
}

void ProvisioningSession::discardCa() {
  if (_caStaging != nullptr) memset(_caStaging, 0, _caCapacity);
  _caLength = 0;
  _caLines = 0;
  _caActive = false;
}

ProvisioningResult ProvisioningSession::handleLine(const char* line,
                                                   size_t length,
                                                   bool writesAllowed,
                                                   uint32_t nowMs,
                                                   ILineSink& out) {
  ProvisioningResult result;
  result.configChanged = false;
  result.showRequested = false;

  if (line == nullptr || length > kMaxLineBytes) {
    out.line("[prov] ERROR: line too long; nothing changed");
    return result;
  }

  Cursor c = {line, line + length};
  const char* token = nullptr;
  size_t tokenLength = 0;
  if (!nextToken(c, token, tokenLength) || !equals(token, tokenLength, "prov")) {
    // Never echo the line: a typo can put a secret where a word was expected.
    out.line("[prov] ERROR: commands start with !prov (see !prov help); "
             "nothing changed");
    return result;
  }
  if (!nextToken(c, token, tokenLength)) {
    help(out);
    return result;
  }

  const bool noArguments = (c.p >= c.end);

  if (equals(token, tokenLength, "help") && noArguments) {
    help(out);
  } else if (equals(token, tokenLength, "ping") && noArguments) {
    out.line("[prov] pong");
  } else if (equals(token, tokenLength, "show") && noArguments) {
    show(out);
    result.showRequested = true;
  } else if (equals(token, tokenLength, "begin") && noArguments) {
    begin(out);
  } else if (equals(token, tokenLength, "abort") && noArguments) {
    abort(out);
  } else if (equals(token, tokenLength, "commit") && noArguments) {
    result.configChanged = commit(writesAllowed, out);
  } else if (equals(token, tokenLength, "set")) {
    set(false, c.p, static_cast<size_t>(c.end - c.p), out);
  } else if (equals(token, tokenLength, "sethex")) {
    set(true, c.p, static_cast<size_t>(c.end - c.p), out);
  } else if (equals(token, tokenLength, "ca")) {
    result.configChanged =
        handleCa(c.p, static_cast<size_t>(c.end - c.p), writesAllowed, out);
  } else if (equals(token, tokenLength, "reset")) {
    if (noArguments) {
      requestReset(nowMs, out);
    } else {
      const char* confirm = nullptr;
      size_t confirmLength = 0;
      const char* code = nullptr;
      size_t codeLength = 0;
      if (nextToken(c, confirm, confirmLength) &&
          equals(confirm, confirmLength, "confirm") &&
          nextToken(c, code, codeLength) && c.p >= c.end) {
        result.configChanged =
            confirmReset(code, codeLength, writesAllowed, nowMs, out);
      } else {
        out.line("[prov] ERROR: use !prov reset, then !prov reset confirm "
                 "<code>; nothing changed");
      }
    }
  } else {
    out.line("[prov] ERROR: unknown command or unexpected arguments "
             "(see !prov help); nothing changed");
  }
  return result;
}

void ProvisioningSession::help(ILineSink& out) {
  out.line("[prov] commands (values are never printed back):");
  out.line("[prov]   !prov show                  stored (and staged) "
           "configuration, redacted");
  out.line("[prov]   !prov begin                 start editing a copy of the "
           "stored configuration");
  out.line("[prov]   !prov set <field> <value>   fields: ssid pass url id "
           "token; value = rest of the line");
  out.line("[prov]   !prov sethex <field> <hex>  the same, value hex-encoded");
  out.line("[prov]   !prov commit                validate and store "
           "atomically, then apply");
  out.line("[prov]   !prov abort                 discard staged changes");
  out.line("[prov]   !prov reset                 erase TTH Bot settings "
           "(asks for a confirmation code)");
  out.line("[prov]   !prov ca begin | ca line <pem line> | ca commit | ca abort "
           "| ca clear   the gateway CA (public)");
}

void ProvisioningSession::show(ILineSink& out) {
  reportConfig("stored", _store.active(), _store.hasConfig(), out);
  if (_store.hasConfig()) {
    emit(out, "[config] stored generation %lu in slot %c",
         static_cast<unsigned long>(_store.generation()), _store.activeSlot());
  }
  if (_trust != nullptr) reportTrustAnchor("stored", *_trust, _certCheck, out);
  if (_staging) reportConfig("staged", _staged, true, out);
  if (_caActive) {
    emit(out, "[config] staged ca: %u line(s), %u bytes",
         static_cast<unsigned>(_caLines), static_cast<unsigned>(_caLength));
  }
}

void ProvisioningSession::begin(ILineSink& out) {
  config::clear(_staged);
  if (_store.hasConfig()) {
    _staged = _store.active();
    emit(out, "[prov] staging started from the stored configuration "
              "(generation %lu)",
         static_cast<unsigned long>(_store.generation()));
  } else {
    out.line("[prov] staging started (empty: nothing stored yet)");
  }
  _staging = true;
}

void ProvisioningSession::abort(ILineSink& out) {
  if (!_staging) {
    out.line("[prov] nothing staged");
    return;
  }
  config::clear(_staged);
  _staging = false;
  out.line("[prov] staging discarded; stored configuration unchanged");
}

void ProvisioningSession::confirmStaged(config::Field field, ILineSink& out) {
  using config::Field;
  switch (field) {
    case Field::Ssid:
      emit(out, "[prov] staged ssid: %u bytes",
           static_cast<unsigned>(_staged.ssidLength));
      return;
    case Field::Password:
      out.line(_staged.passwordLength == 0 ? "[prov] staged pass: set (open network)"
                                           : "[prov] staged pass: set");
      return;
    case Field::GatewayUrl: {
      config::GatewayUrlParts parts;
      config::checkGatewayUrl(_staged.gatewayUrl, _staged.gatewayUrlLength, &parts);
      emit(out, "[prov] staged url: wss host=%.64s port=%u path=%.48s", parts.host,
           static_cast<unsigned>(parts.port), parts.path);
      return;
    }
    case Field::DeviceId:
      emit(out, "[prov] staged id: %s", _staged.deviceId);
      return;
    case Field::DeviceToken:
      emit(out, "[prov] staged token: %u chars",
           static_cast<unsigned>(_staged.deviceTokenLength));
      return;
  }
}

void ProvisioningSession::set(bool hex, const char* rest, size_t restLength,
                              ILineSink& out) {
  if (!_staging) {
    out.line("[prov] ERROR: nothing staged - send !prov begin first; nothing "
             "changed");
    return;
  }

  Cursor c = {rest, rest + restLength};
  const char* name = nullptr;
  size_t nameLength = 0;
  config::Field field = config::Field::Ssid;
  if (!nextToken(c, name, nameLength) ||
      !config::fieldFromName(name, nameLength, field)) {
    out.line("[prov] ERROR: unknown field (ssid, pass, url, id, token); "
             "nothing changed");
    return;
  }

  const char* value = c.p;
  size_t valueLength = static_cast<size_t>(c.end - c.p);
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(value);

  if (hex) {
    if (valueLength % 2 != 0) {
      emit(out, "[prov] ERROR set %s: hex value has an odd length; nothing changed",
           config::toString(field));
      return;
    }
    if (valueLength / 2 > sizeof(_decoded)) {
      emit(out, "[prov] ERROR set %s: too long; nothing changed",
           config::toString(field));
      return;
    }
    for (size_t i = 0; i < valueLength; i += 2) {
      const int hi = hexNibble(value[i]);
      const int lo = hexNibble(value[i + 1]);
      if (hi < 0 || lo < 0) {
        memset(_decoded, 0, sizeof(_decoded));
        emit(out, "[prov] ERROR set %s: not hexadecimal; nothing changed",
             config::toString(field));
        return;
      }
      _decoded[i / 2] = static_cast<uint8_t>((hi << 4) | lo);
    }
    bytes = _decoded;
    valueLength /= 2;
  }

  config::Check why = config::Check::Ok;
  const bool accepted = config::setField(_staged, field, bytes, valueLength, &why);
  memset(_decoded, 0, sizeof(_decoded));
  if (!accepted) {
    emit(out, "[prov] ERROR set %s: %s; nothing changed", config::toString(field),
         config::toString(why));
    return;
  }
  confirmStaged(field, out);
}

bool ProvisioningSession::commit(bool writesAllowed, ILineSink& out) {
  if (!_staging) {
    out.line("[prov] ERROR commit: nothing staged; stored configuration "
             "unchanged");
    return false;
  }
  if (!writesAllowed) {
    out.line("[prov] ERROR commit: busy (a turn or playback is active); "
             "staging kept - retry when idle");
    return false;
  }

  config::Field problem = config::Field::Ssid;
  config::Check why = config::Check::Ok;
  const CommitResult result = _store.commit(_staged, &problem, &why);
  switch (result) {
    case CommitResult::Ok:
      emit(out, "[prov] COMMITTED generation %lu to slot %c; applying",
           static_cast<unsigned long>(_store.generation()), _store.activeSlot());
      config::clear(_staged);
      _staging = false;
      return true;
    case CommitResult::Incomplete:
      emit(out, "[prov] ERROR commit: incomplete (%s: %s); stored "
                "configuration unchanged; staging kept",
           config::toString(problem), config::toString(why));
      return false;
    case CommitResult::GenerationExhausted:
    case CommitResult::WriteFailed:
    case CommitResult::VerifyFailed:
      emit(out, "[prov] ERROR commit: %s; stored configuration unchanged "
                "(%s); staging kept",
           toString(result), _store.hasConfig() ? "previous generation still live"
                                               : "nothing stored");
      return false;
  }
  return false;
}

bool ProvisioningSession::handleCa(const char* rest, size_t restLength,
                                   bool writesAllowed, ILineSink& out) {
  if (_trust == nullptr || _caStaging == nullptr) {
    out.line("[prov] ERROR: CA commands are not available in this build");
    return false;
  }
  Cursor c = {rest, rest + restLength};
  const char* word = nullptr;
  size_t wordLength = 0;
  if (!nextToken(c, word, wordLength)) {
    out.line("[prov] ERROR: use !prov ca begin | ca line <text> | ca commit | "
             "ca abort | ca clear; nothing changed");
    return false;
  }
  const bool noArguments = (c.p >= c.end);

  if (equals(word, wordLength, "begin") && noArguments) {
    discardCa();
    _caActive = true;
    out.line("[prov] CA staging started: send each PEM line with !prov ca line "
             "<text>, then !prov ca commit");
    return false;
  }

  if (equals(word, wordLength, "line")) {
    if (!_caActive) {
      out.line("[prov] ERROR ca line: nothing staged - send !prov ca begin first");
      return false;
    }
    const char* value = c.p;
    const size_t valueLength = static_cast<size_t>(c.end - c.p);
    if (valueLength == 0 || valueLength > trust::kMaxPemLineBytes) {
      emit(out, "[prov] ERROR ca line: 1-%u characters; CA staging kept",
           static_cast<unsigned>(trust::kMaxPemLineBytes));
      return false;
    }
    for (size_t i = 0; i < valueLength; ++i) {
      if (value[i] < 0x20 || value[i] > 0x7E) {
        out.line("[prov] ERROR ca line: invalid character; CA staging kept");
        return false;
      }
    }
    if (_caLength + valueLength + 1 > trust::kMaxPemBytes ||
        _caLength + valueLength + 2 > _caCapacity) {
      discardCa();
      out.line("[prov] ERROR ca line: CA bundle larger than 4096 bytes; CA "
               "staging discarded");
      return false;
    }
    memcpy(_caStaging + _caLength, value, valueLength);
    _caLength += valueLength;
    _caStaging[_caLength++] = '\n';
    _caStaging[_caLength] = '\0';
    ++_caLines;
    emit(out, "[prov] ca line %u (%u bytes)", static_cast<unsigned>(_caLines),
         static_cast<unsigned>(_caLength));
    return false;
  }

  if (equals(word, wordLength, "abort") && noArguments) {
    if (!_caActive) {
      out.line("[prov] no CA staged");
    } else {
      discardCa();
      out.line("[prov] CA staging discarded; stored CA unchanged");
    }
    return false;
  }

  if (equals(word, wordLength, "commit") && noArguments) {
    if (!_caActive) {
      out.line("[prov] ERROR ca commit: nothing staged; stored CA unchanged");
      return false;
    }
    if (!writesAllowed) {
      out.line("[prov] ERROR ca commit: busy (a turn or playback is active); CA "
               "staging kept - retry when idle");
      return false;
    }
    uint8_t certificates = 0;
    const trust::PemCheck check =
        trust::checkPemBundle(_caStaging, _caLength, &certificates);
    if (check != trust::PemCheck::Ok) {
      emit(out, "[prov] ERROR ca commit: %s; stored CA unchanged; CA staging kept",
           trust::toString(check));
      return false;
    }
    char summary[96] = {0};
    if (_certCheck != nullptr &&
        !_certCheck->check(_caStaging, _caLength, summary, sizeof(summary))) {
      emit(out, "[prov] ERROR ca commit: refused by the TLS parser (%.96s); stored "
                "CA unchanged; CA staging kept",
           summary);
      return false;
    }
    const CommitResult result = _trust->commit(_caStaging, _caLength, nullptr);
    if (result != CommitResult::Ok) {
      emit(out, "[prov] ERROR ca commit: %s; stored CA unchanged; CA staging kept",
           toString(result));
      return false;
    }
    emit(out,
         "[prov] CA COMMITTED generation %lu to slot %c (%u certificate(s), %u "
         "bytes); applying",
         static_cast<unsigned long>(_trust->generation()), _trust->activeSlot(),
         static_cast<unsigned>(_trust->certificates()),
         static_cast<unsigned>(_trust->length()));
    discardCa();
    return true;
  }

  if (equals(word, wordLength, "clear") && noArguments) {
    if (!writesAllowed) {
      out.line("[prov] ERROR ca clear: busy (a turn or playback is active); "
               "retry when idle");
      return false;
    }
    const CommitResult result = _trust->clear();
    if (result != CommitResult::Ok) {
      emit(out, "[prov] ERROR ca clear: %s; stored CA unchanged", toString(result));
      return false;
    }
    discardCa();
    out.line("[prov] CA CLEARED: the gateway connection stays disabled until a "
             "CA is provisioned");
    return true;
  }

  out.line("[prov] ERROR: unknown ca command (begin, line, commit, abort, "
           "clear); nothing changed");
  return false;
}

void ProvisioningSession::requestReset(uint32_t nowMs, ILineSink& out) {
  _resetCode = static_cast<uint16_t>(1000u + (_random.next() % 9000u));
  _resetDeadlineMs = nowMs + kResetConfirmWindowMs;
  _resetPending = true;
  emit(out, "[prov] RESET requested: erases Wi-Fi, gateway, CA and device identity "
            "(NVS namespace tth only). Confirm within 30 s: !prov reset confirm %u",
       static_cast<unsigned>(_resetCode));
}

bool ProvisioningSession::confirmReset(const char* code, size_t codeLength,
                                       bool writesAllowed, uint32_t nowMs,
                                       ILineSink& out) {
  if (!_resetPending) {
    out.line("[prov] ERROR: no reset pending - send !prov reset first; nothing "
             "changed");
    return false;
  }
  if (static_cast<int32_t>(nowMs - _resetDeadlineMs) >= 0) {
    _resetPending = false;
    out.line("[prov] ERROR: confirmation code expired; reset cancelled, nothing "
             "changed");
    return false;
  }

  uint32_t value = 0;
  bool numeric = codeLength == 4;
  for (size_t i = 0; numeric && i < codeLength; ++i) {
    if (code[i] < '0' || code[i] > '9') numeric = false;
    value = value * 10u + static_cast<uint32_t>(code[i] - '0');
  }
  if (!numeric || value != _resetCode) {
    _resetPending = false;
    out.line("[prov] ERROR: wrong confirmation code; reset cancelled, nothing "
             "changed");
    return false;
  }
  if (!writesAllowed) {
    out.line("[prov] ERROR reset: busy (a turn or playback is active); retry "
             "the confirmation when idle");
    return false;
  }

  _resetPending = false;
  if (!_store.reset()) {
    out.line("[prov] ERROR reset: erase failed; stored configuration unchanged");
    return false;
  }
  // The namespace erase removed the CA slots too.
  if (_trust != nullptr) _trust->forget();
  discardCa();
  config::clear(_staged);
  _staging = false;
  out.line("[prov] RESET done: TTH Bot configuration erased; Wi-Fi stopped");
  return true;
}

}  // namespace tth
