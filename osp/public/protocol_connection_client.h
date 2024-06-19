// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef OSP_PUBLIC_PROTOCOL_CONNECTION_CLIENT_H_
#define OSP_PUBLIC_PROTOCOL_CONNECTION_CLIENT_H_

#include <memory>
#include <ostream>
#include <string>

#include "osp/public/instance_request_ids.h"
#include "osp/public/message_demuxer.h"
#include "osp/public/protocol_connection.h"
#include "osp/public/protocol_connection_service_observer.h"
#include "osp/public/service_listener.h"
#include "platform/base/error.h"
#include "platform/base/ip_address.h"
#include "platform/base/macros.h"

namespace openscreen::osp {

// Embedder's view of the network service that initiates OSP connections to OSP
// receivers.
//
// NOTE: This API closely resembles that for the ProtocolConnectionServer; the
// client currently lacks Suspend(). Consider factoring out a common
// ProtocolConnectionEndpoint when the two APIs are finalized.
class ProtocolConnectionClient : public ServiceListener::Observer {
 public:
  enum class State { kStopped = 0, kStarting, kRunning, kStopping };

  class ConnectionRequestCallback {
   public:
    virtual ~ConnectionRequestCallback() = default;

    // Called when a new connection was created between 5-tuples.
    virtual void OnConnectionOpened(
        uint64_t request_id,
        std::unique_ptr<ProtocolConnection> connection) = 0;
    virtual void OnConnectionFailed(uint64_t request_id) = 0;
  };

  class ConnectRequest {
   public:
    ConnectRequest();
    ConnectRequest(ProtocolConnectionClient* parent, uint64_t request_id);
    ConnectRequest(ConnectRequest&& other) noexcept;
    ~ConnectRequest();
    ConnectRequest& operator=(ConnectRequest&& other) noexcept;

    // This returns true for a valid and in progress ConnectRequest.
    // MarkComplete is called and this returns false when the request
    // completes.
    explicit operator bool() const { return request_id_; }

    uint64_t request_id() const { return request_id_; }

    // Records that the requested connect operation is complete so it doesn't
    // need to attempt a cancel on destruction.
    void MarkComplete() { request_id_ = 0; }

   private:
    ProtocolConnectionClient* parent_ = nullptr;
    // The |request_id_| of a valid ConnectRequest should be greater than 0.
    uint64_t request_id_ = 0;
  };

  virtual ~ProtocolConnectionClient();

  // Starts the client using the config object.
  // Returns true if state() == kStopped and the service will be started,
  // false otherwise.
  virtual bool Start() = 0;

  // NOTE: Currently we do not support Suspend()/Resume() for the connection
  // client.  Add those if we can define behavior for the OSP protocol and QUIC
  // for those operations.
  // See: https://github.com/webscreens/openscreenprotocol/issues/108

  // Stops listening and cancels any search in progress.
  // Returns true if state() != (kStopped|kStopping).
  virtual bool Stop() = 0;

  // Open a new connection to `instance_name`.  This may succeed synchronously
  // if there are already connections open to `instance_name`, otherwise it will
  // be asynchronous. Returns true if succeed synchronously or asynchronously,
  // false otherwise. `request` is overwritten with the result of a successful
  // connection attempt.
  virtual bool Connect(const std::string& instance_name,
                       ConnectRequest& request,
                       ConnectionRequestCallback* request_callback) = 0;

  // Synchronously open a new connection to an instance identified by
  // `instance_id`.  Returns nullptr if it can't be completed synchronously
  // (e.g. there are no existing open connections to that instance).
  virtual std::unique_ptr<ProtocolConnection> CreateProtocolConnection(
      uint64_t instance_id) = 0;

  MessageDemuxer* message_demuxer() const { return &demuxer_; }

  InstanceRequestIds* instance_request_ids() { return &instance_request_ids_; }

  // Returns the current state of the listener.
  State state() const { return state_; }

  // Returns the last error reported by this client.
  const Error& last_error() const { return last_error_; }

 protected:
  ProtocolConnectionClient(MessageDemuxer& demuxer,
                           ProtocolConnectionServiceObserver& observer);

  virtual void CancelConnectRequest(uint64_t request_id) = 0;

  State state_ = State::kStopped;
  Error last_error_;
  MessageDemuxer& demuxer_;
  InstanceRequestIds instance_request_ids_;
  ProtocolConnectionServiceObserver& observer_;

  OSP_DISALLOW_COPY_AND_ASSIGN(ProtocolConnectionClient);
};

std::ostream& operator<<(std::ostream& os,
                         ProtocolConnectionClient::State state);

}  // namespace openscreen::osp

#endif  // OSP_PUBLIC_PROTOCOL_CONNECTION_CLIENT_H_
