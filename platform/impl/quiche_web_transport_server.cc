// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/impl/quiche_web_transport_server.h"

#include <openssl/evp.h>

#include <utility>

#include "platform/impl/quic/quic_alarm_factory_impl.h"
#include "platform/impl/quic/quic_packet_writer_impl.h"
#include "platform/impl/quic/quic_utils.h"
#include "platform/impl/quiche_web_transport_session.h"
#include "quiche/quic/core/crypto/proof_source_x509.h"
#include "quiche/quic/core/http/web_transport_only_server_session.h"
#include "quiche/quic/core/quic_default_clock.h"
#include "quiche/quic/core/quic_default_connection_helper.h"
#include "quiche/quic/core/quic_utils.h"
#include "quiche/quic/core/quic_version_manager.h"
#include "quiche/quic/core/quic_versions.h"
#include "util/base64.h"
#include "util/crypto/certificate_utils.h"
#include "util/crypto/sha2.h"
#include "util/osp_logging.h"
#include "util/raw_ref.h"

namespace openscreen {

namespace {

constexpr int kRsaKeySize = 2048;
constexpr std::chrono::days kCertValidityDuration{7};
constexpr char kCertCommonName[] = "dummy.cast.local";

struct GeneratedCert {
  std::vector<std::string> certificates;
  bssl::UniquePtr<EVP_PKEY> private_key;
  std::string fingerprint;
};

std::optional<GeneratedCert> GenerateSelfSignedCertForQuic() {
  auto key_pair = GenerateRsaKeyPair(kRsaKeySize);
  if (!key_pair) {
    OSP_LOG_ERROR << "Failed to generate RSA key pair";
    return std::nullopt;
  }

  auto cert_or_error = CreateSelfSignedX509Certificate(
      kCertCommonName, kCertValidityDuration, *key_pair);
  if (cert_or_error.is_error()) {
    OSP_LOG_ERROR << "Failed to create self-signed cert: "
                  << cert_or_error.error();
    return std::nullopt;
  }

  auto der_or_error = ExportX509CertificateToDer(*cert_or_error.value());
  if (der_or_error.is_error()) {
    OSP_LOG_ERROR << "Failed to export cert to DER: " << der_or_error.error();
    return std::nullopt;
  }

  std::vector<uint8_t> der_cert = std::move(der_or_error.value());
  std::string der_cert_str(der_cert.begin(), der_cert.end());

  auto fingerprint_or_error = SHA256HashString(der_cert_str);
  if (fingerprint_or_error.is_error()) {
    OSP_LOG_ERROR << "Failed to hash certificate: "
                  << fingerprint_or_error.error();
    return std::nullopt;
  }
  std::string fingerprint = base64::Encode(fingerprint_or_error.value());

  return GeneratedCert{
      .certificates = {std::move(der_cert_str)},
      .private_key = std::move(key_pair),
      .fingerprint = std::move(fingerprint),
  };
}

}  // namespace

class QuicheWebTransportServer::ServerDispatcher : public quic::QuicDispatcher {
 public:
  using OnSessionCreatedCallback =
      std::function<void(std::unique_ptr<WebTransportSession>)>;

  ServerDispatcher(const quic::QuicConfig* config,
                   const quic::QuicCryptoServerConfig* crypto_config,
                   std::unique_ptr<quic::QuicVersionManager> version_manager,
                   std::unique_ptr<quic::QuicConnectionHelperInterface> helper,
                   std::unique_ptr<quic::QuicAlarmFactory> alarm_factory,
                   quic::ConnectionIdGeneratorInterface& generator,
                   TaskRunner& task_runner,
                   OnSessionCreatedCallback on_session_created)
      : quic::QuicDispatcher(config,
                             crypto_config,
                             version_manager.get(),
                             std::move(helper),
                             /*session_helper=*/nullptr,
                             std::move(alarm_factory),
                             quic::kQuicDefaultConnectionIdLength,
                             generator),
        version_manager_(std::move(version_manager)),
        task_runner_(task_runner),
        on_session_created_(std::move(on_session_created)) {}

 protected:
  absl::StatusOr<quic::WebTransportConnectResponse> HandleIncomingRequest(
      webtransport::Session* raw_session,
      const quic::WebTransportIncomingRequestDetails& details) {
    std::unique_ptr<webtransport::SessionVisitor> visitor_proxy;
    auto session_wrapper = std::make_unique<QuicheWebTransportSession>(
        raw_session, *task_runner_, visitor_proxy);

    on_session_created_(std::move(session_wrapper));

    quic::WebTransportConnectResponse response;
    response.visitor = std::move(visitor_proxy);
    return response;
  }

  std::unique_ptr<quic::QuicSession> CreateQuicSession(
      quic::QuicConnectionId connection_id,
      const quic::QuicSocketAddress& self_address,
      const quic::QuicSocketAddress& peer_address,
      std::string_view alpn,
      const quic::ParsedQuicVersion& version,
      const quic::ParsedClientHello& parsed_chlo,
      quic::ConnectionIdGeneratorInterface& connection_id_generator) override {
    auto connection = std::make_unique<quic::QuicConnection>(
        connection_id, self_address, peer_address, helper(), alarm_factory(),
        writer(), /*owns_writer=*/false, quic::Perspective::IS_SERVER,
        quic::ParsedQuicVersionVector{version}, connection_id_generator);

    auto session = std::make_unique<quic::WebTransportOnlyServerSession>(
        config(), GetSupportedVersions(), connection.release(), this,
        session_helper(), crypto_config(), compressed_certs_cache());

    session->SetHandlerFactory(
        [this](webtransport::Session* raw_session,
               const quic::WebTransportIncomingRequestDetails& details) {
          return HandleIncomingRequest(raw_session, details);
        });

    session->Initialize();
    return session;
  }

 private:
  std::unique_ptr<quic::QuicVersionManager> version_manager_;
  const raw_ref<TaskRunner> task_runner_;
  OnSessionCreatedCallback on_session_created_;
};

QuicheWebTransportServer::QuicheWebTransportServer(TaskRunner& task_runner)
    : task_runner_(task_runner),
      connection_id_generator_(quic::kQuicDefaultConnectionIdLength) {
  helper_ = std::make_unique<quic::QuicDefaultConnectionHelper>();
  alarm_factory_ = std::make_unique<QuicAlarmFactoryImpl>(
      task_runner, quic::QuicDefaultClock::Get());
  supported_versions_ =
      quic::ParsedQuicVersionVector{quic::ParsedQuicVersion::RFCv1()};
}

QuicheWebTransportServer::~QuicheWebTransportServer() {
  OSP_CHECK(task_runner_->IsRunningOnTaskRunner());
  Stop();
}

uint16_t QuicheWebTransportServer::Start(const IPEndpoint& local_endpoint) {
  OSP_CHECK(task_runner_->IsRunningOnTaskRunner());
  auto socket_or = UdpSocket::Create(*task_runner_, this, local_endpoint);
  if (socket_or.is_error()) {
    OSP_LOG_ERROR << "Failed to create server UDP socket: "
                  << socket_or.error();
    return 0;
  }
  socket_ = std::move(socket_or.value());
  socket_->Bind();

  auto cert_data = GenerateSelfSignedCertForQuic();
  if (!cert_data) {
    return 0;
  }
  fingerprint_ = cert_data->fingerprint;

  auto chain = quiche::QuicheReferenceCountedPointer<quic::ProofSource::Chain>(
      new quic::ProofSource::Chain(cert_data->certificates));
  quic::CertificatePrivateKey private_key{std::move(cert_data->private_key)};
  auto proof_source =
      quic::ProofSourceX509::Create(std::move(chain), std::move(private_key));

  crypto_config_ = std::make_unique<quic::QuicCryptoServerConfig>(
      "dummy_source_address_token_secret", quic::QuicRandom::GetInstance(),
      std::move(proof_source), quic::KeyExchangeSource::Default());

  auto version_manager =
      std::make_unique<quic::QuicVersionManager>(supported_versions_);

  dispatcher_ = std::make_unique<ServerDispatcher>(
      &config_, crypto_config_.get(), std::move(version_manager),
      std::make_unique<quic::QuicDefaultConnectionHelper>(),
      std::make_unique<QuicAlarmFactoryImpl>(*task_runner_,
                                             quic::QuicDefaultClock::Get()),
      connection_id_generator_, *task_runner_,
      [this](std::unique_ptr<WebTransportSession> session) {
        if (delegate_) {
          delegate_->OnSessionCreated(std::move(session));
        }
      });

  auto writer = std::make_unique<PacketWriterImpl>(socket_.get());
  dispatcher_->InitializeWithWriter(writer.release());

  return socket_->GetLocalEndpoint().port;
}

void QuicheWebTransportServer::Stop() {
  OSP_CHECK(task_runner_->IsRunningOnTaskRunner());
  dispatcher_.reset();
  crypto_config_.reset();
  socket_.reset();
}

std::string QuicheWebTransportServer::GetCertificateFingerprint() const {
  return fingerprint_;
}

void QuicheWebTransportServer::OnRead(UdpSocket* socket,
                                      ErrorOr<UdpPacket> packet) {
  if (packet.is_error()) {
    return;
  }
  quic::QuicReceivedPacket quic_packet(
      reinterpret_cast<const char*>(packet.value().data()),
      packet.value().size(), helper_->GetClock()->Now(),
      /*owns_buffer=*/false);

  dispatcher_->ProcessPacket(ToQuicSocketAddress(socket->GetLocalEndpoint()),
                             ToQuicSocketAddress(packet.value().source()),
                             quic_packet);
  dispatcher_->ProcessBufferedChlos(16);
}

void QuicheWebTransportServer::OnError(UdpSocket* socket, const Error& error) {
  OSP_LOG_ERROR << "Server UDP socket error: " << error;
  if (delegate_) {
    delegate_->OnServerClosed();
  }
}

void QuicheWebTransportServer::OnSendError(UdpSocket* socket,
                                           const Error& error) {
  OSP_LOG_ERROR << "Server UDP socket send error: " << error;
}

std::unique_ptr<WebTransportServer> WebTransportServer::Create(
    TaskRunner& task_runner) {
  return std::make_unique<QuicheWebTransportServer>(task_runner);
}

}  // namespace openscreen
