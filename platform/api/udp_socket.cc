// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/api/udp_socket.h"

namespace openscreen {

UdpSocket::UdpSocket() = default;
UdpSocket::~UdpSocket() = default;

// TODO(crbug.com/344896902): Remove these once clients have migrated.
void UdpSocket::SendMessage(const void* data,
                            size_t length,
                            const IPEndpoint& dest) {
  // Must be overriden by subclasses.
  assert(false);
}

void UdpSocket::SendMessage(ByteView data, const IPEndpoint& dest) {
  SendMessage(data.data(), data.size(), dest);
}

UdpSocket::Client::~Client() = default;

}  // namespace openscreen
