#pragma once

#include "tth/Provisioning.h"

namespace tth {

// Parses a CA bundle with the same mbedTLS the TLS client uses, at provisioning
// time, so a bundle that would fail every handshake is refused before it is
// stored. Every certificate must be a CA (basicConstraints CA:TRUE). The
// summary is public data: the certificate count and the first subject.
class MbedCertificateCheck : public ICertificateCheck {
 public:
  bool check(const char* pem, size_t length, char* summary,
             size_t capacity) override;
};

}  // namespace tth
