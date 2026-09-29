// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STREAMING_IMPL_DATA_CHANNEL_SESSION_H_
#define CAST_STREAMING_IMPL_DATA_CHANNEL_SESSION_H_

#include <memory>
#include <string_view>
#include <vector>

#include "cast/streaming/impl/data_channel_stream.h"
#include "platform/api/web_transport.h"
#include "util/raw_ptr.h"
#include "util/weak_ptr.h"

namespace openscreen::cast {

class DataChannelSession final : public WebTransportSession::Delegate,
                                 public DataChannelStream::SessionDelegate {
 public:
  class Visitor {
   public:
    virtual ~Visitor() = default;

    // Called when the peer opens a new channel.
    //
    // The delegate must be set on `channel` before returning: messages that
    // arrived in the same read as the OPEN frame are dispatched as soon as this
    // returns, and a channel with no delegate silently drops them.
    virtual void OnNewInputChannel(std::unique_ptr<DataChannel> channel) = 0;

    virtual void OnConnectionEstablished() = 0;

    // Called when the underlying transport session has closed.
    //
    // This runs inside the transport's own closed-notification, so the
    // implementation must not destroy this DataChannelSession synchronously;
    // post the teardown to a task runner instead.
    virtual void OnConnectionClosed() = 0;
  };

  // Upper bound on simultaneously open channels. Without a cap a peer could
  // open streams until we run out of memory.
  static constexpr size_t kMaxConcurrentChannels = 256;

  DataChannelSession(std::unique_ptr<WebTransportSession> session,
                     Visitor* visitor);

  ~DataChannelSession() override;

  // Register this session as the WebTransportSession delegate and start
  // handling events.
  void Start();

  // Create a new outgoing data channel with the given label. Returns nullptr
  // if the channel could not be created or the OPEN frame could not be sent.
  [[nodiscard]] std::unique_ptr<DataChannel> CreateOutgoingDataChannel(
      std::string_view label);

  // WebTransportSession::Delegate overrides.
  void OnIncomingStream(WebTransportStream* stream) override;
  void OnSessionReady(WebTransportSession* session) override;
  void OnSessionClosed(WebTransportSession* session,
                       const Error& error) override;

  // DataChannelStream::SessionDelegate overrides.
  void OnStreamReady(DataChannelStream* stream) override;
  void OnStreamClosed(DataChannelStream* stream) override;

  size_t GetActiveStreamCountForTesting() const {
    return active_streams_.size();
  }

 private:
  std::unique_ptr<WebTransportSession> session_;
  raw_ptr<Visitor> visitor_;
  std::vector<std::unique_ptr<DataChannelStream>> active_streams_;

  WeakPtrFactory<DataChannelSession> weak_factory_{this};
};

}  // namespace openscreen::cast

#endif  // CAST_STREAMING_IMPL_DATA_CHANNEL_SESSION_H_
