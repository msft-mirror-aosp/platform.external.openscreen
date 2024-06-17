// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "osp/impl/quic/quic_client.h"

#include <algorithm>
#include <functional>
#include <memory>

#include "platform/api/task_runner.h"
#include "platform/api/time.h"
#include "util/osp_logging.h"

namespace openscreen::osp {

QuicClient::QuicClient(
    const EndpointConfig& config,
    MessageDemuxer& demuxer,
    std::unique_ptr<QuicConnectionFactoryClient> connection_factory,
    ProtocolConnectionServiceObserver& observer,
    ClockNowFunctionPtr now_function,
    TaskRunner& task_runner)
    : ProtocolConnectionClient(demuxer, observer),
      connection_factory_(std::move(connection_factory)),
      connection_endpoints_(config.connection_endpoints),
      cleanup_alarm_(now_function, task_runner) {}

QuicClient::~QuicClient() {
  CloseAllConnections();
}

bool QuicClient::Start() {
  if (state_ == State::kRunning)
    return false;
  state_ = State::kRunning;
  Cleanup();  // Start periodic clean-ups.
  observer_.OnRunning();
  return true;
}

bool QuicClient::Stop() {
  if (state_ == State::kStopped)
    return false;
  CloseAllConnections();
  state_ = State::kStopped;
  Cleanup();  // Final clean-up.
  observer_.OnStopped();
  return true;
}

void QuicClient::Cleanup() {
  for (auto& entry : connections_) {
    entry.second.delegate->DestroyClosedStreams();
    if (!entry.second.delegate->has_streams())
      entry.second.connection->Close();
  }

  for (uint64_t instance_number : delete_connections_) {
    auto it = connections_.find(instance_number);
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

bool QuicClient::Connect(const std::string& instance_id,
                         ConnectRequest& request,
                         ConnectionRequestCallback* request_callback) {
  if (state_ != State::kRunning) {
    request_callback->OnConnectionFailed(0);
    OSP_LOG_ERROR << "QuicClient connect failed: QuicClient is not running.";
    return false;
  }
  auto instance_entry = instance_map_.find(instance_id);
  if (instance_entry != instance_map_.end()) {
    auto immediate_result = CreateProtocolConnection(instance_entry->second);
    OSP_CHECK(immediate_result);
    uint64_t request_id = next_request_id_++;
    request = ConnectRequest(this, request_id);
    request_callback->OnConnectionOpened(request_id,
                                         std::move(immediate_result));
    return true;
  }

  return CreatePendingConnection(instance_id, request, request_callback);
}

std::unique_ptr<ProtocolConnection> QuicClient::CreateProtocolConnection(
    uint64_t instance_number) {
  if (state_ != State::kRunning)
    return nullptr;
  auto connection_entry = connections_.find(instance_number);
  if (connection_entry == connections_.end())
    return nullptr;
  return QuicProtocolConnection::FromExisting(
      *this, connection_entry->second.connection.get(),
      connection_entry->second.delegate.get(), instance_number);
}

void QuicClient::OnConnectionDestroyed(QuicProtocolConnection* connection) {
  if (!connection->stream())
    return;

  auto connection_entry = connections_.find(connection->instance_number());
  if (connection_entry == connections_.end())
    return;

  connection_entry->second.delegate->DropProtocolConnection(connection);
}

uint64_t QuicClient::OnCryptoHandshakeComplete(
    ServiceConnectionDelegate* delegate,
    std::string connection_id) {
  const std::string& instance_id = delegate->instance_id();
  auto pending_entry = pending_connections_.find(instance_id);
  if (pending_entry == pending_connections_.end())
    return 0;

  ServiceConnectionData connection_data = std::move(pending_entry->second.data);
  auto* connection = connection_data.connection.get();
  uint64_t instance_number = next_instance_number_++;
  instance_map_[instance_id] = instance_number;
  connections_.emplace(instance_number, std::move(connection_data));

  for (auto& request : pending_entry->second.callbacks) {
    std::unique_ptr<QuicProtocolConnection> pc =
        QuicProtocolConnection::FromExisting(*this, connection, delegate,
                                             instance_number);
    request.second->OnConnectionOpened(request.first, std::move(pc));
  }
  pending_connections_.erase(pending_entry);
  return instance_number;
}

void QuicClient::OnIncomingStream(
    std::unique_ptr<QuicProtocolConnection> connection) {
  // TODO(jophba): Change to just use OnIncomingConnection when the observer
  // is properly set up.
  connection.reset();
}

void QuicClient::OnConnectionClosed(uint64_t instance_number,
                                    std::string connection_id) {
  // TODO(btolsch): Is this how handshake failure is communicated to the
  // delegate?
  auto connection_entry = connections_.find(instance_number);
  if (connection_entry == connections_.end())
    return;
  delete_connections_.push_back(instance_number);

  instance_request_ids_.ResetRequestId(instance_number);
}

void QuicClient::OnDataReceived(uint64_t instance_number,
                                uint64_t protocol_connection_id,
                                const ByteView& bytes) {
  demuxer_.OnStreamData(instance_number, protocol_connection_id, bytes.data(),
                        bytes.size());
}

QuicClient::PendingConnectionData::PendingConnectionData(
    ServiceConnectionData&& data)
    : data(std::move(data)) {}
QuicClient::PendingConnectionData::PendingConnectionData(
    PendingConnectionData&&) noexcept = default;
QuicClient::PendingConnectionData::~PendingConnectionData() = default;
QuicClient::PendingConnectionData& QuicClient::PendingConnectionData::operator=(
    PendingConnectionData&&) noexcept = default;

void QuicClient::OnStarted() {}
void QuicClient::OnStopped() {}
void QuicClient::OnSuspended() {}
void QuicClient::OnSearching() {}

void QuicClient::OnReceiverAdded(const ServiceInfo& info) {
  instance_infos_.insert(std::make_pair(
      info.instance_id,
      InstanceInfo{info.fingerprint, info.v4_endpoint, info.v6_endpoint}));
}

void QuicClient::OnReceiverChanged(const ServiceInfo& info) {
  instance_infos_[info.instance_id] =
      InstanceInfo{info.fingerprint, info.v4_endpoint, info.v6_endpoint};
}

void QuicClient::OnReceiverRemoved(const ServiceInfo& info) {
  instance_infos_.erase(info.instance_id);
}

void QuicClient::OnAllReceiversRemoved() {
  instance_infos_.clear();
}

void QuicClient::OnError(const Error&) {}
void QuicClient::OnMetrics(ServiceListener::Metrics) {}

bool QuicClient::CreatePendingConnection(
    const std::string& instance_id,
    ConnectRequest& request,
    ConnectionRequestCallback* request_callback) {
  auto pending_entry = pending_connections_.find(instance_id);
  if (pending_entry == pending_connections_.end()) {
    uint64_t request_id = StartConnectionRequest(instance_id, request_callback);
    if (request_id) {
      request = ConnectRequest(this, request_id);
      return true;
    } else {
      return false;
    }
  } else {
    uint64_t request_id = next_request_id_++;
    pending_entry->second.callbacks.emplace_back(request_id, request_callback);
    request = ConnectRequest(this, request_id);
    return true;
  }
}

uint64_t QuicClient::StartConnectionRequest(
    const std::string& instance_id,
    ConnectionRequestCallback* request_callback) {
  auto instance_entry = instance_infos_.find(instance_id);
  if (instance_entry == instance_infos_.end()) {
    request_callback->OnConnectionFailed(0);
    OSP_LOG_ERROR << "QuicClient connect failed: can't find information for "
                  << instance_id;
    return 0;
  }

  auto delegate =
      std::make_unique<ServiceConnectionDelegate>(*this, instance_id);
  IPEndpoint endpoint = instance_entry->second.v4_endpoint
                            ? instance_entry->second.v4_endpoint
                            : instance_entry->second.v6_endpoint;
  ErrorOr<std::unique_ptr<QuicConnection>> connection =
      connection_factory_->Connect(connection_endpoints_[0], endpoint,
                                   instance_entry->second.fingerprint,
                                   delegate.get());
  if (!connection) {
    request_callback->OnConnectionFailed(0);
    OSP_LOG_ERROR << "Factory connect failed: " << connection.error();
    return 0;
  }
  auto pending_result = pending_connections_.emplace(
      instance_id, PendingConnectionData(ServiceConnectionData(
                       std::move(connection.value()), std::move(delegate))));
  uint64_t request_id = next_request_id_++;
  pending_result.first->second.callbacks.emplace_back(request_id,
                                                      request_callback);
  return request_id;
}

void QuicClient::CloseAllConnections() {
  for (auto& conn : pending_connections_) {
    conn.second.data.connection->Close();
    for (auto& item : conn.second.callbacks) {
      item.second->OnConnectionFailed(item.first);
    }
  }
  pending_connections_.clear();

  for (auto& conn : connections_) {
    conn.second.connection->Close();
  }
  connections_.clear();

  instance_map_.clear();
  next_instance_number_ = 1;
  instance_request_ids_.Reset();
}

void QuicClient::CancelConnectRequest(uint64_t request_id) {
  for (auto it = pending_connections_.begin(); it != pending_connections_.end();
       ++it) {
    auto& callbacks = it->second.callbacks;
    auto size_before_delete = callbacks.size();
    callbacks.erase(
        std::remove_if(
            callbacks.begin(), callbacks.end(),
            [request_id](const std::pair<uint64_t, ConnectionRequestCallback*>&
                             callback) {
              return request_id == callback.first;
            }),
        callbacks.end());

    if (callbacks.empty()) {
      pending_connections_.erase(it);
      return;
    }

    // If the size of the callbacks vector has changed, we have found the entry
    // and can break out of the loop.
    if (size_before_delete > callbacks.size()) {
      return;
    }
  }
}

}  // namespace openscreen::osp
