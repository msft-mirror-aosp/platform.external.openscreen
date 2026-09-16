// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/impl/quiche_web_transport_client.h"

#include <sstream>
#include <string_view>
#include <utility>

#include "platform/impl/quic/quic_alarm_factory_impl.h"
#include "platform/impl/quic/quic_packet_writer_impl.h"
#include "platform/impl/quic/quic_utils.h"
#include "platform/impl/quiche_web_transport_session.h"
#include "quiche/quic/core/crypto/web_transport_fingerprint_proof_verifier.h"
#include "quiche/quic/core/http/web_transport_http3.h"
#include "quiche/quic/core/quic_default_clock.h"
#include "quiche/quic/core/quic_default_connection_helper.h"
#include "quiche/quic/core/quic_utils.h"
#include "util/base64.h"
#include "util/osp_logging.h"
#include "util/raw_ptr.h"

namespace openscreen {

namespace {

struct ParsedWebTransportUrl {
  IPEndpoint endpoint;
  std::string authority;
  std::string path;
};

std::optional<ParsedWebTransportUrl> ParseWebTransportUrl(
    std::string_view url) {
  constexpr std::string_view kPrefix = "https://";
  if (!url.starts_with(kPrefix)) {
    return std::nullopt;
  }
  std::string_view host_port_path = url.substr(kPrefix.size());
  const size_t path_start = host_port_path.find('/');
  std::string_view host_port;
  std::string_view local_path;
  if (path_start != std::string_view::npos) {
    host_port = host_port_path.substr(0, path_start);
    local_path = host_port_path.substr(path_start);
  } else {
    host_port = host_port_path;
    local_path = "/";
  }

  // NOTE: This client only supports IP address literals for the host.
  // Hostname resolution is not supported.
  auto parsed = IPEndpoint::Parse(host_port);
  if (!parsed) {
    return std::nullopt;
  }
  return ParsedWebTransportUrl{parsed.value(), std::string(host_port),
                               std::string(local_path)};
}

std::unique_ptr<quic::WebTransportFingerprintProofVerifier> CreateProofVerifier(
    const quic::QuicClock* clock,
    std::span<const std::string> server_certificate_hashes) {
  auto proof_verifier =
      std::make_unique<quic::WebTransportFingerprintProofVerifier>(
          clock, /*max_validity_days=*/14);
  for (std::string_view hash_base64 : server_certificate_hashes) {
    std::vector<uint8_t> decoded_hash;
    if (base64::Decode(hash_base64, &decoded_hash)) {
      quic::WebTransportHash hash;
      hash.algorithm = quic::WebTransportHash::kSha256;
      hash.value = std::string(decoded_hash.begin(), decoded_hash.end());
      proof_verifier->AddFingerprint(hash);
    } else {
      OSP_LOG_ERROR << "Failed to decode certificate fingerprint: "
                    << hash_base64;
    }
  }
  return proof_verifier;
}

using OnSettingsReceivedCallback = std::function<void()>;
using OnConnectionClosedCallback =
    std::function<void(quic::QuicErrorCode, std::string_view)>;

class QuicheQuicSpdyClientSession : public quic::QuicSpdyClientSession {
 public:
  QuicheQuicSpdyClientSession(
      const quic::QuicConfig& config,
      const quic::ParsedQuicVersionVector& supported_versions,
      std::unique_ptr<quic::QuicConnection> connection,
      const quic::QuicServerId& server_id,
      quic::QuicCryptoClientConfig* crypto_config,
      OnSettingsReceivedCallback on_settings_received,
      OnConnectionClosedCallback on_connection_closed)
      : quic::QuicSpdyClientSession(config,
                                    supported_versions,
                                    connection.release(),
                                    server_id,
                                    crypto_config),
        on_settings_received_(std::move(on_settings_received)),
        on_connection_closed_(std::move(on_connection_closed)) {}

  bool OnSettingsFrame(const quic::SettingsFrame& frame) override {
    bool result = quic::QuicSpdyClientSession::OnSettingsFrame(frame);
    if (SupportsWebTransport() && on_settings_received_) {
      on_settings_received_();
    }
    return result;
  }

  void OnConnectionClosed(const quic::QuicConnectionCloseFrame& frame,
                          quic::ConnectionCloseSource source) override {
    quic::QuicSpdyClientSession::OnConnectionClosed(frame, source);
    if (on_connection_closed_) {
      on_connection_closed_(frame.quic_error_code, frame.error_details);
    }
  }

  quic::WebTransportHttp3VersionSet LocallySupportedWebTransportVersions()
      const override {
    return quic::kDefaultSupportedWebTransportVersions;
  }

  quic::HttpDatagramSupport LocalHttpDatagramSupport() override {
    return quic::HttpDatagramSupport::kRfcAndDraft04;
  }

 private:
  OnSettingsReceivedCallback on_settings_received_;
  OnConnectionClosedCallback on_connection_closed_;
};

}  // namespace

class QuicheWebTransportClient::ConnectingVisitor
    : public WebTransportSession::Delegate {
 public:
  ConnectingVisitor(QuicheWebTransportClient* client,
                    std::unique_ptr<QuicheWebTransportSession> session,
                    ConnectCallback callback)
      : client_(client),
        session_(std::move(session)),
        callback_(std::move(callback)) {
    session_->SetDelegate(this);
  }

  void OnSessionReady(WebTransportSession* session) override {
    OSP_LOG_INFO << "ConnectingVisitor OnSessionReady called!";
    session_->SetDelegate(nullptr);
    auto cb = std::move(callback_);
    std::unique_ptr<WebTransportSession> session_base = std::move(session_);
    client_->OnConnectionComplete(this);
    cb(std::move(session_base));
  }

  void OnSessionClosed(WebTransportSession* session,
                       const Error& error) override {
    OSP_LOG_INFO << "ConnectingVisitor OnSessionClosed called! error=" << error;
    auto cb = std::move(callback_);
    client_->OnConnectionComplete(this);
    cb(error);
  }

  void OnIncomingStream(WebTransportStream* stream) override {}

 private:
  raw_ptr<QuicheWebTransportClient> client_;
  std::unique_ptr<QuicheWebTransportSession> session_;
  ConnectCallback callback_;
};

QuicheWebTransportClient::QuicheWebTransportClient(TaskRunner& task_runner)
    : task_runner_(task_runner), connection_id_generator_(0) {
  helper_ = std::make_unique<quic::QuicDefaultConnectionHelper>();
  alarm_factory_ = std::make_unique<QuicAlarmFactoryImpl>(
      task_runner, quic::QuicDefaultClock::Get());
  supported_versions_ =
      quic::ParsedQuicVersionVector{quic::ParsedQuicVersion::RFCv1()};
}

QuicheWebTransportClient::~QuicheWebTransportClient() {
  OSP_CHECK(task_runner_->IsRunningOnTaskRunner());
}

void QuicheWebTransportClient::Connect(std::string_view url,
                                       const WebTransportOptions& options,
                                       ConnectCallback callback) {
  OSP_CHECK(task_runner_->IsRunningOnTaskRunner());
  OSP_CHECK(!socket_);
  const auto parsed_url = ParseWebTransportUrl(url);
  if (!parsed_url) {
    callback(Error(Error::Code::kParameterInvalid, "Invalid URL"));
    return;
  }
  const IPEndpoint& remote_endpoint = parsed_url->endpoint;

  IPEndpoint local_endpoint = remote_endpoint.address.IsV4()
                                  ? IPEndpoint{IPAddress::kAnyV4(), 0}
                                  : IPEndpoint{IPAddress::kAnyV6(), 0};
  auto socket_or = UdpSocket::Create(*task_runner_, this, local_endpoint);
  if (socket_or.is_error()) {
    callback(socket_or.error());
    return;
  }
  socket_ = std::move(socket_or.value());
  socket_->Bind();

  writer_ = std::make_unique<PacketWriterImpl>(socket_.get());

  quic::QuicConnectionId connection_id =
      quic::QuicUtils::CreateRandomConnectionId();
  const quic::QuicSocketAddress remote_address =
      ToQuicSocketAddress(remote_endpoint);

  // We need to bind the socket before we can get its local address.
  const quic::QuicSocketAddress local_address =
      ToQuicSocketAddress(socket_->GetLocalEndpoint());

  auto connection = std::make_unique<quic::QuicConnection>(
      connection_id, local_address, remote_address, helper_.get(),
      alarm_factory_.get(), writer_.get(), /*owns_writer=*/false,
      quic::Perspective::IS_CLIENT, supported_versions_,
      connection_id_generator_);

  crypto_config_ =
      std::make_unique<quic::QuicCryptoClientConfig>(CreateProofVerifier(
          helper_->GetClock(), options.server_certificate_hashes));

  std::ostringstream host_ss;
  host_ss << remote_endpoint.address;
  const quic::QuicServerId server_id(host_ss.str(), remote_endpoint.port);

  std::string authority = parsed_url->authority;
  std::string path = parsed_url->path;

  pending_callback_ = callback;

  session_ = std::make_unique<QuicheQuicSpdyClientSession>(
      config_, supported_versions_, std::move(connection), server_id,
      crypto_config_.get(),
      [this, authority = std::move(authority), path = std::move(path),
       callback]() { InitiateWebTransport(authority, path, callback); },
      [this, callback](quic::QuicErrorCode error_code,
                       std::string_view error_details) {
        OnSessionConnectionClosed(callback, error_code, error_details);
      });
  session_->Initialize();
  session_->CryptoConnect();
}

void QuicheWebTransportClient::InitiateWebTransport(std::string_view authority,
                                                    std::string_view path,
                                                    ConnectCallback callback) {
  pending_callback_ = nullptr;
  auto* stream = session_->CreateOutgoingBidirectionalStream();
  if (!stream) {
    callback(Error(Error::Code::kConnectionFailed,
                   "Failed to create CONNECT stream"));
    return;
  }

  quiche::HttpHeaderBlock headers;
  headers[":scheme"] = "https";
  headers[":authority"] = authority;
  headers[":path"] = path;
  headers[":method"] = "CONNECT";
  headers[":protocol"] = "webtransport";

  stream->WriteHeaders(std::move(headers), /*fin=*/false, nullptr);

  auto* quiche_session = stream->web_transport();
  if (!quiche_session) {
    callback(Error(Error::Code::kConnectionFailed,
                   "Failed to get WebTransport session"));
    return;
  }

  auto session_wrapper =
      std::make_unique<QuicheWebTransportSession>(quiche_session,
                                                  *task_runner_);

  auto connecting_visitor = std::make_unique<ConnectingVisitor>(
      this, std::move(session_wrapper), std::move(callback));
  connecting_visitors_.push_back(std::move(connecting_visitor));
}

void QuicheWebTransportClient::OnSessionConnectionClosed(
    ConnectCallback callback,
    quic::QuicErrorCode error_code,
    std::string_view error_details) {
  if (pending_callback_) {
    pending_callback_ = nullptr;
    callback(Error(
        Error::Code::kConnectionFailed,
        std::format("QUIC connection closed before WebTransport initiation: "
                    "{} - {}",
                    static_cast<int>(error_code), error_details)));
  }
}

void QuicheWebTransportClient::OnRead(UdpSocket* socket,
                                      ErrorOr<UdpPacket> packet) {
  if (packet.is_error()) {
    return;
  }
  quic::QuicReceivedPacket quic_packet(
      reinterpret_cast<const char*>(packet.value().data()),
      packet.value().size(), helper_->GetClock()->Now(),
      /*owns_buffer=*/false);

  session_->connection()->ProcessUdpPacket(
      ToQuicSocketAddress(socket->GetLocalEndpoint()),
      ToQuicSocketAddress(packet.value().source()), quic_packet);
}

void QuicheWebTransportClient::OnError(UdpSocket* socket, const Error& error) {
  if (session_ && session_->connection()->connected()) {
    session_->connection()->CloseConnection(
        quic::QUIC_CONNECTION_CANCELLED, "Socket error",
        quic::ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET);
  }
}

void QuicheWebTransportClient::OnSendError(UdpSocket* socket,
                                           const Error& error) {
  OnError(socket, error);
}

void QuicheWebTransportClient::OnConnectionComplete(
    ConnectingVisitor* visitor) {
  task_runner_->PostTask([weak_this = weak_factory_.GetWeakPtr(), visitor]() {
    if (!weak_this) {
      return;
    }
    auto& visitors = weak_this->connecting_visitors_;
    auto it =
        std::find_if(visitors.begin(), visitors.end(),
                     [visitor](const std::unique_ptr<ConnectingVisitor>& v) {
                       return v.get() == visitor;
                     });
    if (it != visitors.end()) {
      visitors.erase(it);
    }
  });
}

std::unique_ptr<WebTransportClient> WebTransportClient::Create(
    TaskRunner& task_runner) {
  return std::make_unique<QuicheWebTransportClient>(task_runner);
}

}  // namespace openscreen
