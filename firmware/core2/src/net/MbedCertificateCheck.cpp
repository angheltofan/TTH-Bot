#include "net/MbedCertificateCheck.h"

#include <mbedtls/x509_crt.h>

#include <stdio.h>

namespace tth {

bool MbedCertificateCheck::check(const char* pem, size_t length, char* summary,
                                 size_t capacity) {
  char scratch[8];
  if (summary == nullptr || capacity == 0) {
    summary = scratch;
    capacity = sizeof(scratch);
  }
  summary[0] = '\0';
  // mbedTLS parses PEM only when the terminating NUL is part of the length.
  if (pem == nullptr || length == 0 || pem[length] != '\0') {
    snprintf(summary, capacity, "not a NUL-terminated PEM bundle");
    return false;
  }

  mbedtls_x509_crt chain;
  mbedtls_x509_crt_init(&chain);
  const int ret =
      mbedtls_x509_crt_parse(&chain, reinterpret_cast<const unsigned char*>(pem), length + 1);

  bool ok = false;
  if (ret < 0) {
    snprintf(summary, capacity, "parse error -0x%04x", static_cast<unsigned>(-ret));
  } else if (ret > 0) {
    snprintf(summary, capacity, "%d certificate(s) did not parse", ret);
  } else {
    unsigned count = 0;
    bool allCa = true;
    for (const mbedtls_x509_crt* c = &chain; c != nullptr && c->raw.len > 0; c = c->next) {
      ++count;
      if (!c->ca_istrue) allCa = false;
    }
    if (count == 0) {
      snprintf(summary, capacity, "no certificate");
    } else if (!allCa) {
      snprintf(summary, capacity, "not a CA certificate (basicConstraints CA:TRUE required)");
    } else {
      char subject[72];
      if (mbedtls_x509_dn_gets(subject, sizeof(subject), &chain.subject) < 0) {
        snprintf(subject, sizeof(subject), "?");
      }
      snprintf(summary, capacity, "%u certificate(s), first subject %s", count, subject);
      ok = true;
    }
  }
  mbedtls_x509_crt_free(&chain);
  return ok;
}

}  // namespace tth
