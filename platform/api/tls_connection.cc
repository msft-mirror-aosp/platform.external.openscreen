// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/api/tls_connection.h"

namespace openscreen {

TlsConnection::TlsConnection() = default;
TlsConnection::~TlsConnection() = default;

// TODO(crbug.com/344896902): Remove these once clients have migrated.
[[nodiscard]] bool TlsConnection::Send(const void* data, size_t len) {
  // Must be overridden by subclasses.
  assert(false);
  return false;
}

[[nodiscard]] bool TlsConnection::Send(ByteView data) {
  return Send(data.data(), data.size());
}

TlsConnection::Client::~Client() = default;

}  // namespace openscreen
