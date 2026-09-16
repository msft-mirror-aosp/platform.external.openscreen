// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PLATFORM_IMPL_QUICHE_WEB_TRANSPORT_CLIENT_H_
#define PLATFORM_IMPL_QUICHE_WEB_TRANSPORT_CLIENT_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "platform/api/task_runner.h"
#include "platform/api/udp_socket.h"
#include "platform/api/web_transport.h"
#include "quiche/quic/core/deterministic_connection_id_generator.h"
#include "quiche/quic/core/http/quic_spdy_client_session.h"
#include "quiche/quic/core/quic_config.h"
#include "quiche/quic/core/quic_connection.h"
#include "quiche/quic/core/quic_error_codes.h"
#include "util/raw_ref.h"
#include "util/weak_ptr.h"

namespace openscreen {

// QUICHE-based implementation of WebTransportClient. Manages the client socket
// lifecycle, handshake, and spins up a QuicSpdyClientSession to establish a
// WebTransport session.
class QuicheWebTransportClient : public WebTransportClient,
                                 public UdpSocket::Client {
 public:
  explicit QuicheWebTransportClient(TaskRunner& task_runner);
  ~QuicheWebTransportClient() override;

  QuicheWebTransportClient(const QuicheWebTransportClient&) = delete;
  QuicheWebTransportClient& operator=(const QuicheWebTransportClient&) = delete;
  QuicheWebTransportClient(QuicheWebTransportClient&&) noexcept = delete;
  QuicheWebTransportClient& operator=(QuicheWebTransportClient&&) noexcept =
      delete;

  // WebTransportClient implementation.
  void Connect(std::string_view url,
               const WebTransportOptions& options,
               ConnectCallback callback) override;

  // UdpSocket::Client implementation.
  void OnRead(UdpSocket* socket, ErrorOr<UdpPacket> packet) override;
  void OnError(UdpSocket* socket, const Error& error) override;
  void OnSendError(UdpSocket* socket, const Error& error) override;

 private:
  class ConnectingVisitor;

  void OnConnectionComplete(ConnectingVisitor* visitor);
  void InitiateWebTransport(std::string_view authority,
                            std::string_view path,
                            ConnectCallback callback);
  void OnSessionConnectionClosed(ConnectCallback callback,
                                 quic::QuicErrorCode error_code,
                                 std::string_view error_details);

  const raw_ref<TaskRunner> task_runner_;
  std::unique_ptr<quic::QuicConnectionHelperInterface> helper_;
  std::unique_ptr<quic::QuicAlarmFactory> alarm_factory_;
  quic::DeterministicConnectionIdGenerator connection_id_generator_;
  quic::ParsedQuicVersionVector supported_versions_;
  quic::QuicConfig config_;

  std::unique_ptr<UdpSocket> socket_;
  std::unique_ptr<quic::QuicPacketWriter> writer_;
  std::unique_ptr<quic::QuicSpdyClientSession> session_;
  std::unique_ptr<quic::QuicCryptoClientConfig> crypto_config_;

  std::vector<std::unique_ptr<ConnectingVisitor>> connecting_visitors_;

  ConnectCallback pending_callback_;

  WeakPtrFactory<QuicheWebTransportClient> weak_factory_{this};
};

}  // namespace openscreen

#endif  // PLATFORM_IMPL_QUICHE_WEB_TRANSPORT_CLIENT_H_
