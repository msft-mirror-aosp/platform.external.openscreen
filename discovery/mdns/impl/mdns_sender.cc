// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "discovery/mdns/impl/mdns_sender.h"

#include <algorithm>
#include <iostream>
#include <vector>

#include "discovery/mdns/public/mdns_constants.h"
#if defined(USE_RUST_MDNS_PARSER)
#include "discovery/mdns/public/simple_mdns_writer.h"
#else
#include "discovery/mdns/public/mdns_writer.h"
#endif
#include "platform/api/udp_socket.h"
#include "platform/base/span.h"
#include "platform/base/udp_packet.h"

namespace openscreen::discovery {

MdnsSender::MdnsSender(UdpSocket& socket) : socket_(socket) {}

MdnsSender::~MdnsSender() = default;

Error MdnsSender::SendMulticast(const MdnsMessage& message) {
  const IPEndpoint& endpoint = socket_->IsIPv6() ? kMulticastSendIPv6Endpoint
                                                 : kMulticastSendIPv4Endpoint;
  return SendMessage(message, endpoint);
}

Error MdnsSender::SendMessage(const MdnsMessage& message,
                              const IPEndpoint& endpoint) {
#if defined(USE_RUST_MDNS_PARSER)
  static_assert(kMaxMulticastMessageSize <= UdpPacket::kUdpMaxPacketSize);
  ErrorOr<std::vector<uint8_t>> bytes = SimpleMdnsWriter::Write(message);
  if (!bytes) {
    return bytes.error();
  }
  const size_t max_size = (endpoint == kMulticastSendIPv4Endpoint ||
                           endpoint == kMulticastSendIPv6Endpoint ||
                           endpoint == kDefaultSiteLocalGroupIPv4Endpoint ||
                           endpoint == kDefaultSiteLocalGroupIPv6Endpoint)
                              ? kMaxMulticastMessageSize
                              : UdpPacket::kUdpMaxPacketSize;
  if (bytes.value().size() > max_size) {
    return Error::Code::kInsufficientBuffer;
  }

  socket_->SendMessage(ByteView(bytes.value()), endpoint);
  return Error::Code::kNone;
#else
  // Always try to write the message into the buffer even if MaxWireSize is
  // greater than maximum message size. Domain name compression might reduce the
  // on-the-wire size of the message sufficiently for it to fit into the buffer.
  std::vector<uint8_t> buffer(
      std::min(message.MaxWireSize(), kMaxMulticastMessageSize));
  MdnsWriter writer(buffer.data(), buffer.size());
  if (!writer.Write(message)) {
    return Error::Code::kInsufficientBuffer;
  }

  socket_->SendMessage(ByteView(buffer.data(), writer.offset()), endpoint);
  return Error::Code::kNone;
#endif
}

void MdnsSender::OnSendError(UdpSocket* socket, const Error& error) {
  OSP_LOG_ERROR << "Error sending packet " << error;
}

}  // namespace openscreen::discovery
