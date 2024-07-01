// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "osp/impl/quic/quic_server.h"

#include <functional>
#include <utility>

#include "util/osp_logging.h"

namespace openscreen::osp {

// static
QuicAgentCertificate& QuicServer::GetAgentCertificate() {
  static QuicAgentCertificate agent_certificate;
  return agent_certificate;
}

QuicServer::QuicServer(
    const ServiceConfig& config,
    MessageDemuxer& demuxer,
    std::unique_ptr<QuicConnectionFactoryServer> connection_factory,
    ProtocolConnectionServiceObserver& observer,
    ClockNowFunctionPtr now_function,
    TaskRunner& task_runner)
    : QuicServiceBase(config, demuxer, observer, now_function, task_runner),
      instance_name_(config.instance_name),
      instance_request_ids_(InstanceRequestIds::Role::kServer),
      connection_factory_(std::move(connection_factory)) {}

QuicServer::~QuicServer() {
  CloseAllConnections();
}

bool QuicServer::Start() {
  bool result = StartImpl();
  if (result) {
    connection_factory_->SetServerDelegate(this, connection_endpoints_);
  }
  return result;
}

bool QuicServer::Stop() {
  bool result = StopImpl();
  if (result) {
    connection_factory_->SetServerDelegate(nullptr, {});
  }
  return result;
}

bool QuicServer::Suspend() {
  return SuspendImpl();
}

bool QuicServer::Resume() {
  return ResumeImpl();
}

ProtocolConnectionEndpoint::State QuicServer::GetState() {
  return state_;
}

MessageDemuxer& QuicServer::GetMessageDemuxer() {
  return demuxer_;
}

InstanceRequestIds& QuicServer::GetInstanceRequestIds() {
  return instance_request_ids_;
}

std::unique_ptr<ProtocolConnection> QuicServer::CreateProtocolConnection(
    uint64_t instance_id) {
  return CreateProtocolConnectionImpl(instance_id);
}

std::string QuicServer::GetAgentFingerprint() {
  return GetAgentCertificate().GetAgentFingerprint();
}

uint64_t QuicServer::OnCryptoHandshakeComplete(
    ServiceConnectionDelegate* delegate) {
  OSP_CHECK_EQ(state_, State::kRunning);

  const std::string& instance_name = delegate->instance_name();
  auto pending_entry = pending_connections_.find(instance_name);
  if (pending_entry == pending_connections_.end()) {
    return 0;
  }

  ServiceConnectionData connection_data = std::move(pending_entry->second);
  pending_connections_.erase(pending_entry);
  uint64_t instance_id = next_instance_id_++;
  instance_map_[instance_name] = instance_id;
  connections_.emplace(instance_id, std::move(connection_data));
  return instance_id;
}

void QuicServer::OnConnectionClosed(uint64_t instance_id) {
  QuicServiceBase::OnConnectionClosed(instance_id);
  instance_request_ids_.ResetRequestId(instance_id);
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
