// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "osp/impl/quic/certificates/quic_agent_certificate.h"

#include <utility>

#include "quiche/quic/core/quic_utils.h"
#include "util/base64.h"
#include "util/crypto/pem_helpers.h"
#include "util/osp_logging.h"
#include "util/read_file.h"

namespace openscreen::osp {

namespace {

// TODO(issuetracker.google.com/300236996): Replace with OSP certificate
// generation. Fixed agent certificate is used currently.
//
// NOTE: This should not be used for any end-user software, as the private key
// is obviously not private now.
constexpr char kCertificatesPath[] =
    "osp/impl/quic/certificates/openscreen.pem";
constexpr char kPrivateKeyPath[] = "osp/impl/quic/certificates/openscreen.key";

}  // namespace

QuicAgentCertificate::QuicAgentCertificate() {
  bool result = LoadCredentials();
  OSP_CHECK(result);
}

QuicAgentCertificate::~QuicAgentCertificate() = default;

bool QuicAgentCertificate::LoadAgentCertificate(std::string_view filename) {
  certificates_.clear();
  agent_fingerprint_.clear();

  // NOTE: There are currently some spec issues about certificates that are
  // still under discussion. Add validations to check if this is a valid OSP
  // agent certificate once all the issues are closed.
  certificates_ = ReadCertificatesFromPemFile(filename);
  if (!certificates_.empty()) {
    agent_fingerprint_ = base64::Encode(quic::RawSha256(certificates_[0]));
    return !agent_fingerprint_.empty();
  } else {
    return false;
  }
}

bool QuicAgentCertificate::LoadPrivateKey(std::string_view filename) {
  key_raw_.clear();
  key_raw_ = ReadEntireFileToString(filename);
  return !key_raw_.empty();
}

// NOTE: OSP certificate generation is not implemented yet and fixed certificate
// is used currently. So rotate agent certificate is not supported now.
bool QuicAgentCertificate::RotateAgentCertificate() {
  OSP_NOTREACHED();
}

AgentFingerprint QuicAgentCertificate::GetAgentFingerprint() {
  return agent_fingerprint_;
}

std::unique_ptr<quic::ProofSource> QuicAgentCertificate::CreateProofSource() {
  if (certificates_.empty() || key_raw_.empty() || agent_fingerprint_.empty()) {
    return nullptr;
  }

  auto chain = quiche::QuicheReferenceCountedPointer<quic::ProofSource::Chain>(
      new quic::ProofSource::Chain(certificates_));
  OSP_CHECK(chain) << "Failed to create the quic::ProofSource::Chain.";

  std::unique_ptr<quic::CertificatePrivateKey> key =
      quic::CertificatePrivateKey::LoadFromDer(key_raw_);
  OSP_CHECK(key) << "Failed to parse the key file.";

  return quic::ProofSourceX509::Create(std::move(chain), std::move(*key));
}

void QuicAgentCertificate::ResetCredentials() {
  agent_fingerprint_.clear();
  certificates_.clear();
  key_raw_.clear();
}

bool QuicAgentCertificate::LoadCredentials() {
  bool result = LoadAgentCertificate(kCertificatesPath) &&
                LoadPrivateKey(kPrivateKeyPath);
  if (!result) {
    ResetCredentials();
    return false;
  } else {
    return true;
  }
}

}  // namespace openscreen::osp
