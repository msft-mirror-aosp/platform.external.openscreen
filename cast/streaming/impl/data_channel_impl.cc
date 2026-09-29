// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/streaming/impl/data_channel_impl.h"

namespace openscreen::cast {

DataChannelImpl::DataChannelImpl(DataChannelStream* stream) : stream_(stream) {
  if (stream_) {
    stream_->SetDelegate(this);
    cached_label_ = stream_->label();
    cached_state_ = stream_->state();
  } else {
    cached_state_ = State::kClosed;
  }
}

DataChannelImpl::~DataChannelImpl() {
  if (stream_) {
    stream_->SetDelegate(nullptr);
    // Close() may destroy the stream. Clear the pointer so it is never left
    // dangling at destruction, which BackupRefPtr treats as an error.
    DataChannelStream* stream = stream_;
    stream_ = nullptr;
    stream->Close();
  }
}

Error DataChannelImpl::Send(ByteView data) {
  if (!stream_) {
    return Error(Error::Code::kConnectionFailed, "Underlying stream destroyed");
  }
  return stream_->Send(data);
}

void DataChannelImpl::Close() {
  if (stream_) {
    stream_->Close();
  } else {
    cached_state_ = State::kClosed;
    if (delegate_) {
      delegate_->OnStateChange(cached_state_);
    }
  }
}

std::string_view DataChannelImpl::label() const {
  return stream_ ? stream_->label() : cached_label_;
}

DataChannel::State DataChannelImpl::state() const {
  return stream_ ? stream_->state() : cached_state_;
}

void DataChannelImpl::SetDelegate(DataChannel::Delegate* delegate) {
  delegate_ = delegate;
  if (delegate_) {
    delegate_->OnStateChange(state());
  }
}

void DataChannelImpl::OnMessage(ByteView data) {
  if (delegate_) {
    delegate_->OnMessage(data);
  }
}

void DataChannelImpl::OnStateChange(DataChannel::State state) {
  // An application that attaches its delegate from within
  // Visitor::OnNewInputChannel() has already been told the current state by
  // SetDelegate(); the stream then reports it a second time as it unwinds out
  // of OnStreamReady(). States only ever advance, so suppressing a repeat of
  // the state we already published collapses that duplicate.
  if (cached_state_ == state) {
    return;
  }
  cached_state_ = state;
  if (delegate_) {
    delegate_->OnStateChange(state);
  }
}

void DataChannelImpl::OnWriteReady() {
  if (delegate_) {
    delegate_->OnWriteReady();
  }
}

void DataChannelImpl::OnDestroyed() {
  stream_ = nullptr;
  OnStateChange(State::kClosed);
}

}  // namespace openscreen::cast
