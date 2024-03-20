// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "osp/impl/quic/quic_connection_factory_impl.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "osp/impl/quic/open_screen_client_session.h"
#include "osp/impl/quic/open_screen_server_session.h"
#include "osp/impl/quic/quic_alarm_factory_impl.h"
#include "osp/impl/quic/quic_connection_impl.h"
#include "osp/impl/quic/quic_dispatcher_impl.h"
#include "osp/impl/quic/quic_packet_writer_impl.h"
#include "osp/impl/quic/quic_utils.h"
#include "platform/api/task_runner.h"
#include "platform/api/time.h"
#include "platform/base/error.h"
#include "quiche/common/quiche_random.h"
#include "quiche/quic/core/crypto/certificate_view.h"
#include "quiche/quic/core/crypto/proof_source_x509.h"
#include "quiche/quic/core/crypto/quic_compressed_certs_cache.h"
#include "quiche/quic/core/crypto/web_transport_fingerprint_proof_verifier.h"
#include "quiche/quic/core/quic_default_connection_helper.h"
#include "quiche/quic/core/quic_utils.h"
#include "quiche/quic/core/quic_version_manager.h"
#include "quiche/quic/platform/api/quic_socket_address.h"
#include "util/crypto/pem_helpers.h"
#include "util/osp_logging.h"
#include "util/read_file.h"
#include "util/std_util.h"
#include "util/trace_logging.h"

namespace openscreen::osp {

namespace {

constexpr char kSourceAddressTokenSecret[] = "secret";
constexpr size_t kMaxConnectionsToCreate = 256;
constexpr char kFingerPrint[] =
    "50:87:8D:CA:1B:9B:67:76:CB:87:88:1C:43:20:82:7A:91:F5:9B:74:4D:85:95:D0:"
    "76:E6:0B:50:7F:D3:29:D9";
constexpr char kCertificatesPath[] =
    "osp/impl/quic/certificates/openscreen.pem";
constexpr char kPrivateKeyPath[] = "osp/impl/quic/certificates/openscreen.key";

// TODO(issuetracker.google.com/300236996): Replace with OSP certificate
// generation.
std::unique_ptr<quic::ProofSource> CreateProofSource() {
  std::vector<std::string> certificates =
      ReadCertificatesFromPemFile(kCertificatesPath);
  OSP_CHECK_EQ(certificates.size(), 1u)
      << "Failed to parse the certificates file.";
  auto chain = quiche::QuicheReferenceCountedPointer<quic::ProofSource::Chain>(
      new quic::ProofSource::Chain(std::move(certificates)));
  OSP_CHECK(chain) << "Failed to create the quic::ProofSource::Chain.";

  const std::string key_raw = ReadEntireFileToString(kPrivateKeyPath);
  std::unique_ptr<quic::CertificatePrivateKey> key =
      quic::CertificatePrivateKey::LoadFromDer(key_raw);
  OSP_CHECK(key) << "Failed to parse the key file.";

  return quic::ProofSourceX509::Create(std::move(chain), std::move(*key));
}

}  // namespace

QuicConnectionFactoryImpl::QuicConnectionFactoryImpl(TaskRunner& task_runner)
    : task_runner_(task_runner) {
  helper_ = std::make_unique<quic::QuicDefaultConnectionHelper>();
  alarm_factory_ = std::make_unique<QuicAlarmFactoryImpl>(
      task_runner, quic::QuicDefaultClock::Get());
}

QuicConnectionFactoryImpl::~QuicConnectionFactoryImpl() {
  for (auto& it : connections_) {
    it.second.connection->Close();
  }
}

void QuicConnectionFactoryImpl::SetServerDelegate(
    ServerDelegate* delegate,
    const std::vector<IPEndpoint>& endpoints) {
  OSP_CHECK(!delegate != !server_delegate_);

  server_delegate_ = delegate;
  sockets_.reserve(sockets_.size() + endpoints.size());

  crypto_server_config_ = std::make_unique<quic::QuicCryptoServerConfig>(
      kSourceAddressTokenSecret, quic::QuicRandom::GetInstance(),
      CreateProofSource(), quic::KeyExchangeSource::Default());

  for (const auto& endpoint : endpoints) {
    // TODO(mfoltz): Need to notify the caller and/or ServerDelegate if socket
    // create/bind errors occur. Maybe return an Error immediately, and undo
    // partial progress (i.e. "unwatch" all the sockets and call
    // sockets_.clear() to close the sockets)?
    auto create_result = UdpSocket::Create(task_runner_, this, endpoint);
    if (!create_result) {
      OSP_LOG_ERROR << "failed to create socket (for " << endpoint
                    << "): " << create_result.error().message();
      continue;
    }
    std::unique_ptr<UdpSocket> server_socket = std::move(create_result.value());
    server_socket->Bind();

    auto dispatcher = std::make_unique<QuicDispatcherImpl>(
        &config_, crypto_server_config_.get(),
        std::make_unique<quic::QuicVersionManager>(supported_versions_),
        std::make_unique<quic::QuicDefaultConnectionHelper>(),
        std::make_unique<OpenScreenCryptoServerStreamHelper>(),
        std::make_unique<QuicAlarmFactoryImpl>(task_runner_,
                                               quic::QuicDefaultClock::Get()),
        quic::kQuicDefaultConnectionIdLength, connection_id_generator_, *this);
    quic::QuicPacketWriter* writer = new PacketWriterImpl(server_socket.get());
    dispatcher->InitializeWithWriter(writer);
    dispatcher->ProcessBufferedChlos(kMaxConnectionsToCreate);
    dispatchers_.insert(
        std::make_pair(server_socket.get(), std::move(dispatcher)));

    sockets_.emplace_back(std::move(server_socket));
  }
}

void QuicConnectionFactoryImpl::OnError(UdpSocket* socket, Error error) {
  OSP_LOG_ERROR << "failed to configure socket " << error.message();
}

void QuicConnectionFactoryImpl::OnSendError(UdpSocket* socket, Error error) {
  // TODO(crbug.com/openscreen/67): Implement this method.
  OSP_UNIMPLEMENTED();
}

void QuicConnectionFactoryImpl::OnRead(UdpSocket* socket,
                                       ErrorOr<UdpPacket> packet_or_error) {
  TRACE_SCOPED(TraceCategory::kQuic, "QuicConnectionFactoryImpl::OnRead");
  if (packet_or_error.is_error()) {
    TRACE_SET_RESULT(packet_or_error.error());
    return;
  }

  UdpPacket packet = std::move(packet_or_error.value());
  // TODO(btolsch): We will need to rethink this both for ICE and connection
  // migration support.
  auto conn_it = connections_.find(packet.source());
  auto dispatcher_it = dispatchers_.find(socket);
  QuicDispatcherImpl* dispatcher = dispatcher_it != dispatchers_.end()
                                       ? dispatcher_it->second.get()
                                       : nullptr;

  // Return early because no one can process the `packet` in this case.
  if (conn_it == connections_.end() && !dispatcher) {
    return;
  }

  // For QuicServer, the `packet` is passed to corresponding QuicDispatcherImpl.
  // Otherwise, the `packet` is passed to corresponding QuicConnectionImpl.
  if (conn_it == connections_.end() || server_delegate_) {
    // `server_delegate_` shoud not be nullptr for QuicServer.
    OSP_CHECK(server_delegate_);

    if (conn_it == connections_.end()) {
      OSP_VLOG << __func__ << ": QuicDispatcherImpl spawns connection from "
               << packet.source();
    } else {
      OSP_VLOG
          << __func__
          << ": QuicDispatcherImpl processes data for existing connection from "
          << packet.source();
    }

    const quic::QuicReceivedPacket quic_packet(
        reinterpret_cast<const char*>(packet.data()), packet.size(),
        helper_->GetClock()->Now());
    dispatcher->ProcessPacket(ToQuicSocketAddress(socket->GetLocalEndpoint()),
                              ToQuicSocketAddress(packet.source()),
                              quic_packet);
  } else {
    OSP_VLOG
        << __func__
        << ": QuicConnectionImpl processes data for existing connection from "
        << packet.source();
    conn_it->second.connection->OnRead(socket, std::move(packet));
  }
}

std::unique_ptr<QuicConnection> QuicConnectionFactoryImpl::Connect(
    const IPEndpoint& local_endpoint,
    const IPEndpoint& remote_endpoint,
    QuicConnection::Delegate* connection_delegate) {
  auto create_result = UdpSocket::Create(task_runner_, this, local_endpoint);
  if (!create_result) {
    OSP_LOG_ERROR << "failed to create socket: "
                  << create_result.error().message();
    // TODO(mfoltz): This method should return ErrorOr<uni_ptr<QuicConnection>>.
    return nullptr;
  }
  std::unique_ptr<UdpSocket> socket = std::move(create_result.value());
  socket->Bind();

  quic::QuicPacketWriter* writer = new PacketWriterImpl(socket.get());
  quic::QuicConnectionId server_connection_id =
      quic::QuicUtils::CreateRandomConnectionId(helper_->GetRandomGenerator());
  auto connection = std::make_unique<quic::QuicConnection>(
      server_connection_id, ToQuicSocketAddress(local_endpoint),
      ToQuicSocketAddress(remote_endpoint), helper_.get(), alarm_factory_.get(),
      writer, /* owns_writer */ true, quic::Perspective::IS_CLIENT,
      supported_versions_, connection_id_generator_);
  quic::QuicConnectionId client_connection_id =
      quic::QuicUtils::CreateRandomConnectionId(helper_->GetRandomGenerator());
  connection->set_client_connection_id(client_connection_id);

  if (!crypto_client_config_) {
    auto proof_verifier =
        std::make_unique<quic::WebTransportFingerprintProofVerifier>(
            helper_->GetClock(), /*max_validity_days=*/3650);
    const bool success =
        proof_verifier->AddFingerprint(quic::CertificateFingerprint{
            quic::CertificateFingerprint::kSha256, kFingerPrint});
    if (!success) {
      OSP_LOG_ERROR << "Failed to add a certificate fingerprint.";
      return nullptr;
    }
    crypto_client_config_ = std::make_unique<quic::QuicCryptoClientConfig>(
        std::move(proof_verifier), nullptr);
  }

  auto connection_impl = std::make_unique<QuicConnectionImpl>(
      *this, *connection_delegate, *helper_->GetClock());
  // NOTE: Ask the QUICHE authors what quic::QuicServerId to use here for
  // clients that aren't connecting to Internet hosts with a hostname.
  OpenScreenSessionBase* session = new OpenScreenClientSession(
      std::move(connection), *crypto_client_config_, *connection_impl, config_,
      quic::QuicServerId(ToQuicIpAddress(remote_endpoint.address).ToString(),
                         remote_endpoint.port),
      supported_versions_);
  connection_impl->set_session(session, /*owns_session*/ true);

  // TODO(btolsch): This presents a problem for multihomed receivers, which may
  // register as a different endpoint in their response.  I think QUIC is
  // already tolerant of this via connection IDs but this hasn't been tested
  // (and even so, those aren't necessarily stable either).
  connections_.emplace(remote_endpoint,
                       OpenConnection{connection_impl.get(), socket.get()});
  sockets_.emplace_back(std::move(socket));

  return connection_impl;
}

void QuicConnectionFactoryImpl::OnConnectionClosed(QuicConnection* connection) {
  auto entry = std::find_if(
      connections_.begin(), connections_.end(),
      [connection](const decltype(connections_)::value_type& entry) {
        return entry.second.connection == connection;
      });
  OSP_CHECK(entry != connections_.end());
  UdpSocket* const socket = entry->second.socket;
  connections_.erase(entry);

  // If none of the remaining |connections_| reference the socket, close/destroy
  // it.
  if (!ContainsIf(connections_,
                  [socket](const decltype(connections_)::value_type& entry) {
                    return entry.second.socket == socket;
                  })) {
    auto socket_it =
        std::find_if(sockets_.begin(), sockets_.end(),
                     [socket](const std::unique_ptr<UdpSocket>& s) {
                       return s.get() == socket;
                     });
    OSP_CHECK(socket_it != sockets_.end());
    dispatchers_.erase(socket_it->get());
    sockets_.erase(socket_it);
  }
}

}  // namespace openscreen::osp
