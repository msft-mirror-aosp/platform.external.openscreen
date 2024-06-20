// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "osp/public/protocol_connection_endpoint.h"

namespace openscreen::osp {

ProtocolConnectionEndpoint::ProtocolConnectionEndpoint(
    MessageDemuxer& demuxer,
    InstanceRequestIds::Role role,
    ProtocolConnectionServiceObserver& observer)
    : demuxer_(demuxer), instance_request_ids_(role), observer_(observer) {}

ProtocolConnectionEndpoint::~ProtocolConnectionEndpoint() = default;

std::ostream& operator<<(std::ostream& os,
                         ProtocolConnectionEndpoint::State state) {
  switch (state) {
    case ProtocolConnectionEndpoint::State::kStopped:
      return os << "STOPPED";
    case ProtocolConnectionEndpoint::State::kStarting:
      return os << "STARTING";
    case ProtocolConnectionEndpoint::State::kRunning:
      return os << "RUNNING";
    case ProtocolConnectionEndpoint::State::kStopping:
      return os << "STOPPING";
    case ProtocolConnectionEndpoint::State::kSuspended:
      return os << "SUSPENDED";
    default:
      return os << "UNKNOWN";
  }
}

}  // namespace openscreen::osp
