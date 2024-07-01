// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "osp/impl/quic/quic_service_common.h"

#include <memory>
#include <utility>

#include "util/osp_logging.h"

namespace openscreen::osp {

// static
std::unique_ptr<QuicProtocolConnection> QuicProtocolConnection::FromExisting(
    Owner& owner,
    QuicConnection* connection,
    ServiceConnectionDelegate* delegate,
    uint64_t instance_id) {
  OSP_VLOG << "QUIC stream created for instance " << instance_id;
  QuicStream* stream = connection->MakeOutgoingStream(delegate);
  auto pc = std::make_unique<QuicProtocolConnection>(owner, instance_id,
                                                     stream->GetStreamId());
  pc->set_stream(stream);
  delegate->AddStreamPair(ServiceStreamPair{stream, pc->id(), pc.get()});
  return pc;
}

QuicProtocolConnection::QuicProtocolConnection(Owner& owner,
                                               uint64_t instance_id,
                                               uint64_t protocol_connection_id)
    : ProtocolConnection(instance_id, protocol_connection_id), owner_(owner) {}

QuicProtocolConnection::~QuicProtocolConnection() {
  if (stream_) {
    stream_->CloseWriteEnd();
    owner_.OnConnectionDestroyed(this);
    stream_ = nullptr;
  }
}

void QuicProtocolConnection::Write(const ByteView& bytes) {
  if (stream_)
    stream_->Write(bytes);
}

void QuicProtocolConnection::CloseWriteEnd() {
  if (stream_)
    stream_->CloseWriteEnd();
}

void QuicProtocolConnection::OnClose() {
  if (observer_)
    observer_->OnConnectionClosed(*this);
}

ServiceConnectionDelegate::ServiceConnectionDelegate(
    ServiceDelegate& parent,
    const std::string& instance_name)
    : parent_(parent), instance_name_(instance_name) {}

ServiceConnectionDelegate::~ServiceConnectionDelegate() {
  void DestroyClosedStreams();
  OSP_CHECK(streams_.empty());
}

void ServiceConnectionDelegate::AddStreamPair(
    const ServiceStreamPair& stream_pair) {
  const uint64_t stream_id = stream_pair.stream->GetStreamId();
  streams_.emplace(stream_id, stream_pair);
}

void ServiceConnectionDelegate::DropProtocolConnection(
    QuicProtocolConnection* connection) {
  auto stream_entry = streams_.find(connection->stream()->GetStreamId());
  if (stream_entry == streams_.end())
    return;
  stream_entry->second.protocol_connection = nullptr;
}

void ServiceConnectionDelegate::DestroyClosedStreams() {
  closed_streams_.clear();
}

void ServiceConnectionDelegate::OnCryptoHandshakeComplete() {
  instance_id_ = parent_.OnCryptoHandshakeComplete(this);
  OSP_VLOG << "QUIC connection handshake complete for instance "
           << instance_id_;
}

void ServiceConnectionDelegate::OnIncomingStream(QuicStream* stream) {
  OSP_VLOG << "Incoming QUIC stream from instance " << instance_id_;
  pending_connection_->set_stream(stream);
  AddStreamPair(ServiceStreamPair{stream, pending_connection_->id(),
                                  pending_connection_.get()});
  parent_.OnIncomingStream(std::move(pending_connection_));
}

void ServiceConnectionDelegate::OnConnectionClosed() {
  OSP_VLOG << "QUIC connection closed for instance " << instance_id_;
  parent_.OnConnectionClosed(instance_id_);
}

QuicStream::Delegate& ServiceConnectionDelegate::NextStreamDelegate(
    uint64_t stream_id) {
  OSP_CHECK(!pending_connection_);
  pending_connection_ = std::make_unique<QuicProtocolConnection>(
      parent_, instance_id_, stream_id);
  return *this;
}

void ServiceConnectionDelegate::OnReceived(QuicStream* stream,
                                           const ByteView& bytes) {
  auto stream_entry = streams_.find(stream->GetStreamId());
  if (stream_entry == streams_.end())
    return;
  ServiceStreamPair& stream_pair = stream_entry->second;
  parent_.OnDataReceived(instance_id_, stream_pair.protocol_connection_id,
                         bytes);
}

void ServiceConnectionDelegate::OnClose(uint64_t stream_id) {
  OSP_VLOG << "QUIC stream closed for instance " << instance_id_;
  auto stream_entry = streams_.find(stream_id);
  if (stream_entry == streams_.end())
    return;
  ServiceStreamPair& stream_pair = stream_entry->second;
  parent_.OnDataReceived(instance_id_, stream_pair.protocol_connection_id,
                         ByteView());
  if (stream_pair.protocol_connection) {
    stream_pair.protocol_connection->set_stream(nullptr);
    stream_pair.protocol_connection->OnClose();
  }
  // NOTE: If this OnClose is the result of the read end closing when the write
  // end was already closed, there will likely still be a call to OnReceived.
  // We need to delay actually destroying the stream object until the end of the
  // event loop.
  closed_streams_.push_back(std::move(stream_entry->second));
  streams_.erase(stream_entry);
}

}  // namespace openscreen::osp
