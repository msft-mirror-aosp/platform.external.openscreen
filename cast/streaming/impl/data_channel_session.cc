// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/streaming/impl/data_channel_session.h"

#include <limits>
#include <string>
#include <utility>

#include "cast/streaming/impl/data_channel_impl.h"
#include "util/osp_logging.h"

namespace openscreen::cast {

DataChannelSession::DataChannelSession(
    std::unique_ptr<WebTransportSession> session,
    Visitor* visitor)
    : session_(std::move(session)), visitor_(visitor) {}

DataChannelSession::~DataChannelSession() {
  if (session_) {
    session_->SetDelegate(nullptr);
  }
}

void DataChannelSession::Start() {
  if (session_) {
    session_->SetDelegate(this);
  }
}

std::unique_ptr<DataChannel> DataChannelSession::CreateOutgoingDataChannel(
    std::string_view label) {
  if (label.size() > std::numeric_limits<uint16_t>::max()) {
    return nullptr;
  }

  if (!session_) {
    return nullptr;
  }

  if (active_streams_.size() >= kMaxConcurrentChannels) {
    OSP_LOG_ERROR << "Refusing to open channel: already at the "
                  << kMaxConcurrentChannels << " channel limit";
    return nullptr;
  }

  auto stream_or = session_->CreateOutgoingStream();
  if (!stream_or.is_value()) {
    return nullptr;
  }
  auto stream = std::move(stream_or.value());

  auto channel_stream = std::make_unique<DataChannelStream>(
      std::move(stream), std::string(label), this);
  auto* stream_ptr = channel_stream.get();
  WeakPtr<DataChannelStream> weak_stream = stream_ptr->GetWeakPtr();
  active_streams_.push_back(std::move(channel_stream));

  // Only subscribe to transport events once the stream is owned by
  // `active_streams_`; Start() can synchronously re-enter OnStreamClosed().
  stream_ptr->Start();
  if (!weak_stream) {
    return nullptr;
  }

  // On failure SendOpenFrame() closes the channel, which removes (and
  // destroys) it from `active_streams_`.
  if (!stream_ptr->SendOpenFrame()) {
    return nullptr;
  }

  return std::make_unique<DataChannelImpl>(stream_ptr);
}

void DataChannelSession::OnIncomingStream(WebTransportStream* stream) {
  if (active_streams_.size() >= kMaxConcurrentChannels) {
    OSP_LOG_ERROR << "Rejecting incoming channel: already at the "
                  << kMaxConcurrentChannels << " channel limit";
    stream->Close();
    return;
  }

  auto channel_stream = std::make_unique<DataChannelStream>(stream, this);
  auto* stream_ptr = channel_stream.get();
  active_streams_.push_back(std::move(channel_stream));

  // Start() synchronously drains any already-buffered bytes, which can run the
  // whole OPEN frame -> OnStreamReady -> OnNewInputChannel chain and even close
  // the channel again. Registering first keeps OnStreamClosed() able to find
  // this entry, otherwise the stream would leak.
  stream_ptr->Start();
}

void DataChannelSession::OnSessionReady(WebTransportSession* session) {
  if (visitor_) {
    visitor_->OnConnectionEstablished();
  }
}

void DataChannelSession::OnSessionClosed(WebTransportSession* session,
                                         const Error& error) {
  std::vector<std::unique_ptr<DataChannelStream>> streams =
      std::move(active_streams_);
  // A moved-from vector is valid but unspecified, and the callbacks below can
  // re-enter OnStreamClosed(), so put it in a known-empty state first.
  active_streams_.clear();
  WeakPtr<DataChannelSession> weak_this = weak_factory_.GetWeakPtr();
  for (const auto& stream : streams) {
    stream->OnClose(nullptr);
    if (!weak_this) {
      return;
    }
  }

  if (visitor_) {
    visitor_->OnConnectionClosed();
  }
}

void DataChannelSession::OnStreamReady(DataChannelStream* stream) {
  if (visitor_) {
    visitor_->OnNewInputChannel(std::make_unique<DataChannelImpl>(stream));
  }
}

void DataChannelSession::OnStreamClosed(DataChannelStream* stream) {
  std::erase_if(active_streams_,
                [stream](const auto& s) { return s.get() == stream; });
}

}  // namespace openscreen::cast
