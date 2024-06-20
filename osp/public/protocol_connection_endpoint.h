// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef OSP_PUBLIC_PROTOCOL_CONNECTION_ENDPOINT_H_
#define OSP_PUBLIC_PROTOCOL_CONNECTION_ENDPOINT_H_

#include <memory>
#include <ostream>

#include "osp/public/instance_request_ids.h"
#include "osp/public/message_demuxer.h"
#include "osp/public/protocol_connection.h"
#include "osp/public/protocol_connection_service_observer.h"
#include "platform/base/error.h"

namespace openscreen::osp {

// There are two kinds of ProtocolConnectionEndpoints: ProtocolConnectionClient
// and ProtocolConnectionServer. This class holds common codes for the two
// classes.
class ProtocolConnectionEndpoint {
 public:
  enum class State {
    kStopped = 0,
    kStarting,
    kRunning,
    kStopping,
    kSuspended,
  };

  ProtocolConnectionEndpoint(MessageDemuxer& demuxer,
                             InstanceRequestIds::Role role,
                             ProtocolConnectionServiceObserver& observer);
  ProtocolConnectionEndpoint(const ProtocolConnectionEndpoint&) = delete;
  ProtocolConnectionEndpoint& operator=(const ProtocolConnectionEndpoint&) =
      delete;
  ProtocolConnectionEndpoint(ProtocolConnectionEndpoint&&) noexcept = delete;
  ProtocolConnectionEndpoint& operator=(ProtocolConnectionEndpoint&&) noexcept =
      delete;
  virtual ~ProtocolConnectionEndpoint();

  // Returns true if state() == kStopped and the service will start, false
  // otherwise.
  virtual bool Start() = 0;

  // Returns true if state() != (kStopped|kStopping) and the service will stop,
  // false otherwise.
  virtual bool Stop() = 0;

  // Returns true if states() == kRunning and the service will be suspended,
  // false otherwise.
  virtual bool Suspend() = 0;

  // Returns true if states() == kSuspended and the service will start again,
  // false otherwise.
  virtual bool Resume() = 0;

  // Synchronously open a new connection to an instance identified by
  // `instance_id`.  Returns nullptr if it can't be completed synchronously
  // (e.g. there are no existing open connections to that instance).
  virtual std::unique_ptr<ProtocolConnection> CreateProtocolConnection(
      uint64_t instance_id) = 0;

  MessageDemuxer* message_demuxer() const { return &demuxer_; }

  InstanceRequestIds* instance_request_ids() { return &instance_request_ids_; }

  // Returns the current state of the service.
  State state() const { return state_; }

  // Returns the last error reported by this service.
  const Error& last_error() const { return last_error_; }

 protected:
  State state_ = State::kStopped;
  Error last_error_;
  MessageDemuxer& demuxer_;
  InstanceRequestIds instance_request_ids_;
  ProtocolConnectionServiceObserver& observer_;
};

std::ostream& operator<<(std::ostream& os,
                         ProtocolConnectionEndpoint::State state);

}  // namespace openscreen::osp

#endif  // OSP_PUBLIC_PROTOCOL_CONNECTION_ENDPOINT_H_
