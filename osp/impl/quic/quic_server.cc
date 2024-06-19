// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "osp/impl/quic/quic_server.h"

#include <functional>
#include <memory>

#include "platform/api/task_runner.h"
#include "platform/api/time.h"
#include "util/osp_logging.h"

namespace openscreen::osp {

QuicServer::QuicServer(
    const EndpointConfig& config,
    MessageDemuxer& demuxer,
    std::unique_ptr<QuicConnectionFactoryServer> connection_factory,
    ProtocolConnectionServiceObserver& observer,
    ClockNowFunctionPtr now_function,
    TaskRunner& task_runner)
    : ProtocolConnectionServer(demuxer, observer),
      connection_endpoints_(config.connection_endpoints),
      connection_factory_(std::move(connection_factory)),
      cleanup_alarm_(now_function, task_runner) {}

QuicServer::~QuicServer() {
  CloseAllConnections();
}

bool QuicServer::Start() {
  if (state_ != State::kStopped)
    return false;
  state_ = State::kRunning;
  connection_factory_->SetServerDelegate(this, connection_endpoints_);
  Cleanup();  // Start periodic clean-ups.
  observer_.OnRunning();
  return true;
}

bool QuicServer::Stop() {
  if (state_ != State::kRunning && state_ != State::kSuspended)
    return false;
  connection_factory_->SetServerDelegate(nullptr, {});
  CloseAllConnections();
  state_ = State::kStopped;
  Cleanup();  // Final clean-up.
  observer_.OnStopped();
  return true;
}

bool QuicServer::Suspend() {
  // TODO(btolsch): QuicStreams should either buffer or reject writes.
  if (state_ != State::kRunning)
    return false;
  state_ = State::kSuspended;
  observer_.OnSuspended();
  return true;
}

bool QuicServer::Resume() {
  if (state_ != State::kSuspended)
    return false;
  state_ = State::kRunning;
  observer_.OnRunning();
  return true;
}

std::string QuicServer::GetFingerprint() {
  return connection_factory_->GetFingerprint();
}

void QuicServer::Cleanup() {
  for (auto& entry : connections_)
    entry.second.delegate->DestroyClosedStreams();

  for (uint64_t instance_id : delete_connections_) {
    auto it = connections_.find(instance_id);
    if (it != connections_.end()) {
      connections_.erase(it);
    }
  }
  delete_connections_.clear();

  constexpr Clock::duration kQuicCleanupPeriod = std::chrono::milliseconds(500);
  if (state_ != State::kStopped) {
    cleanup_alarm_.ScheduleFromNow([this] { Cleanup(); }, kQuicCleanupPeriod);
  }
}

std::unique_ptr<ProtocolConnection> QuicServer::CreateProtocolConnection(
    uint64_t instance_id) {
  if (state_ != State::kRunning) {
    return nullptr;
  }
  auto connection_entry = connections_.find(instance_id);
  if (connection_entry == connections_.end()) {
    return nullptr;
  }
  return QuicProtocolConnection::FromExisting(
      *this, connection_entry->second.connection.get(),
      connection_entry->second.delegate.get(), instance_id);
}

void QuicServer::OnConnectionDestroyed(QuicProtocolConnection* connection) {
  if (!connection->stream())
    return;

  auto connection_entry = connections_.find(connection->instance_id());
  if (connection_entry == connections_.end())
    return;

  connection_entry->second.delegate->DropProtocolConnection(connection);
}

uint64_t QuicServer::OnCryptoHandshakeComplete(
    ServiceConnectionDelegate* delegate,
    std::string connection_id) {
  OSP_CHECK_EQ(state_, State::kRunning);
  const std::string& instance_name = delegate->instance_name();
  auto pending_entry = pending_connections_.find(instance_name);
  if (pending_entry == pending_connections_.end())
    return 0;
  ServiceConnectionData connection_data = std::move(pending_entry->second);
  pending_connections_.erase(pending_entry);
  uint64_t instance_id = next_instance_id_++;
  instance_map_[instance_name] = instance_id;
  connections_.emplace(instance_id, std::move(connection_data));
  return instance_id;
}

void QuicServer::OnIncomingStream(
    std::unique_ptr<QuicProtocolConnection> connection) {
  OSP_CHECK_EQ(state_, State::kRunning);
  observer_.OnIncomingConnection(std::move(connection));
}

void QuicServer::OnConnectionClosed(uint64_t instance_id,
                                    std::string connection_id) {
  OSP_CHECK_EQ(state_, State::kRunning);
  auto connection_entry = connections_.find(instance_id);
  if (connection_entry == connections_.end())
    return;
  delete_connections_.push_back(instance_id);

  instance_request_ids_.ResetRequestId(instance_id);
}

void QuicServer::OnDataReceived(uint64_t instance_id,
                                uint64_t protocol_connection_id,
                                const ByteView& bytes) {
  OSP_CHECK_EQ(state_, State::kRunning);
  demuxer_.OnStreamData(instance_id, protocol_connection_id, bytes.data(),
                        bytes.size());
}

void QuicServer::CloseAllConnections() {
  for (auto& conn : pending_connections_) {
    conn.second.connection->Close();
  }
  pending_connections_.clear();

  for (auto& conn : connections_) {
    conn.second.connection->Close();
  }
  connections_.clear();

  instance_map_.clear();
  next_instance_id_ = 1u;
  instance_request_ids_.Reset();
}

QuicConnection::Delegate* QuicServer::NextConnectionDelegate(
    const IPEndpoint& source) {
  OSP_CHECK_EQ(state_, State::kRunning);
  OSP_CHECK(!pending_connection_delegate_);
  // NOTE: There is no corresponding instance name for IPEndpoint on the client
  // side. So IPEndpoint is converted into a string and used as instance name.
  pending_connection_delegate_ =
      std::make_unique<ServiceConnectionDelegate>(*this, source.ToString());
  return pending_connection_delegate_.get();
}

void QuicServer::OnIncomingConnection(
    std::unique_ptr<QuicConnection> connection) {
  OSP_CHECK_EQ(state_, State::kRunning);
  const std::string& instance_name =
      pending_connection_delegate_->instance_name();
  pending_connections_.emplace(
      instance_name,
      ServiceConnectionData(std::move(connection),
                            std::move(pending_connection_delegate_)));
}

}  // namespace openscreen::osp
