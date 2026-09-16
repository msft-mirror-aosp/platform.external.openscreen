// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PLATFORM_IMPL_QUICHE_WEB_TRANSPORT_SERVER_H_
#define PLATFORM_IMPL_QUICHE_WEB_TRANSPORT_SERVER_H_

#include <memory>
#include <string>
#include <vector>

#include "platform/api/task_runner.h"
#include "platform/api/udp_socket.h"
#include "platform/api/web_transport.h"
#include "quiche/quic/core/deterministic_connection_id_generator.h"
#include "quiche/quic/core/quic_config.h"
#include "quiche/quic/core/quic_dispatcher.h"
#include "util/raw_ptr.h"
#include "util/raw_ref.h"

namespace openscreen {

// QUICHE-based implementation of WebTransportServer. Generates a self-signed
// certificate on startup, starts listening on a UDP socket, and dispatches
// incoming QUIC packets to QuicDispatcher to create WebTransport sessions.
class QuicheWebTransportServer : public WebTransportServer,
                                 public UdpSocket::Client {
 public:
  explicit QuicheWebTransportServer(TaskRunner& task_runner);
  ~QuicheWebTransportServer() override;

  QuicheWebTransportServer(const QuicheWebTransportServer&) = delete;
  QuicheWebTransportServer& operator=(const QuicheWebTransportServer&) = delete;
  QuicheWebTransportServer(QuicheWebTransportServer&&) noexcept = delete;
  QuicheWebTransportServer& operator=(QuicheWebTransportServer&&) noexcept =
      delete;

  // WebTransportServer implementation.
  void SetDelegate(Delegate* delegate) override { delegate_ = delegate; }
  [[nodiscard]] uint16_t Start(const IPEndpoint& local_endpoint) override;
  void Stop() override;
  [[nodiscard]] std::string GetCertificateFingerprint() const override;

  // UdpSocket::Client implementation.
  void OnRead(UdpSocket* socket, ErrorOr<UdpPacket> packet) override;
  void OnError(UdpSocket* socket, const Error& error) override;
  void OnSendError(UdpSocket* socket, const Error& error) override;

 private:
  class ServerDispatcher;

  const raw_ref<TaskRunner> task_runner_;
  raw_ptr<Delegate> delegate_;
  std::string fingerprint_;

  std::unique_ptr<UdpSocket> socket_;
  std::unique_ptr<quic::QuicConnectionHelperInterface> helper_;
  std::unique_ptr<quic::QuicAlarmFactory> alarm_factory_;
  quic::DeterministicConnectionIdGenerator connection_id_generator_;
  quic::ParsedQuicVersionVector supported_versions_;
  quic::QuicConfig config_;
  std::unique_ptr<quic::QuicCryptoServerConfig> crypto_config_;

  std::unique_ptr<ServerDispatcher> dispatcher_;
};

}  // namespace openscreen

#endif  // PLATFORM_IMPL_QUICHE_WEB_TRANSPORT_SERVER_H_
