// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/crypto/certificate_utils.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <chrono>

#include "gtest/gtest.h"
#include "platform/api/time.h"
#include "platform/base/error.h"
#include "util/std_util.h"

namespace openscreen {
namespace {

constexpr char kName[] = "test.com";
constexpr auto kDuration = std::chrono::seconds(31556952);

TEST(CertificateUtilTest, CreatesValidCertificate) {
  bssl::UniquePtr<EVP_PKEY> pkey = GenerateRsaKeyPair();
  ASSERT_TRUE(pkey);

  ErrorOr<bssl::UniquePtr<X509>> certificate =
      CreateSelfSignedX509Certificate(kName, kDuration, *pkey);
  ASSERT_TRUE(certificate.is_value());

  // Validate the generated certificate.
  EXPECT_NE(0, X509_verify(certificate.value().get(), pkey.get()));
}

TEST(CertificateUtilTest, ExportsAndImportsCertificate) {
  bssl::UniquePtr<EVP_PKEY> pkey = GenerateRsaKeyPair();
  ASSERT_TRUE(pkey);
  ErrorOr<bssl::UniquePtr<X509>> certificate =
      CreateSelfSignedX509Certificate(kName, kDuration, *pkey);
  ASSERT_TRUE(certificate.is_value());

  ErrorOr<std::vector<uint8_t>> exported =
      ExportX509CertificateToDer(*certificate.value());
  ASSERT_TRUE(exported.is_value()) << exported.error();
  EXPECT_FALSE(exported.value().empty());

  ErrorOr<bssl::UniquePtr<X509>> imported =
      ImportCertificate(exported.value().data(), exported.value().size());
  ASSERT_TRUE(imported.is_value()) << imported.error();
  ASSERT_TRUE(imported.value().get());

  // Validate the imported certificate.
  EXPECT_NE(0, X509_verify(imported.value().get(), pkey.get()));
}

TEST(CertificateUtilTest, ImportRSAPrivateKeyInvalidData) {
  EXPECT_EQ(Error::Code::kParameterInvalid,
            ImportRSAPrivateKey(nullptr, 100).error().code());
  EXPECT_EQ(Error::Code::kParameterInvalid,
            ImportRSAPrivateKey(reinterpret_cast<const uint8_t*>("foo"), 0)
                .error()
                .code());

  const uint8_t kInvalidKeyData[] = {0x00, 0x01, 0x02, 0x03};
  ErrorOr<bssl::UniquePtr<EVP_PKEY>> result =
      ImportRSAPrivateKey(kInvalidKeyData, sizeof(kInvalidKeyData));
  EXPECT_TRUE(result.is_error());
  EXPECT_EQ(Error::Code::kRSAKeyParseError, result.error().code());
}

TEST(CertificateUtilTest, GetSpkiTlvHandlesEmptyCert) {
  bssl::UniquePtr<X509> empty_cert(X509_new());
  ASSERT_TRUE(empty_cert);
  EXPECT_TRUE(GetSpkiTlv(empty_cert.get()).empty());
}

}  // namespace
}  // namespace openscreen
