// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/streaming/impl/data_channel_stream.h"

#include <limits>
#include <utility>

#include "util/osp_logging.h"

namespace openscreen::cast {

DataChannelStream::DataChannelStream(std::unique_ptr<WebTransportStream> stream,
                                     std::string label,
                                     SessionDelegate* session_delegate)
    : owned_stream_(std::move(stream)),
      stream_(owned_stream_.get()),
      label_(std::move(label)),
      state_(DataChannel::State::kConnecting),
      session_delegate_(session_delegate) {}

DataChannelStream::DataChannelStream(WebTransportStream* stream,
                                     SessionDelegate* session_delegate)
    : stream_(stream),
      state_(DataChannel::State::kConnecting),
      session_delegate_(session_delegate) {}

DataChannelStream::~DataChannelStream() {
  if (stream_) {
    stream_->SetDelegate(nullptr);
  }
  if (delegate_) {
    delegate_->OnDestroyed();
  }
}

void DataChannelStream::Start() {
  if (stream_) {
    stream_->SetDelegate(this);
  }
}

void DataChannelStream::SetDelegate(Delegate* delegate) {
  delegate_ = delegate;
  if (delegate_ && state_ == DataChannel::State::kOpen) {
    delegate_->OnStateChange(state_);
  }
}

Error DataChannelStream::Send(ByteView data) {
  if (state_ != DataChannel::State::kOpen) {
    return Error(Error::Code::kConnectionFailed, "Channel not open");
  }

  // Keep the sender within the same limit the receiver enforces, so we never
  // emit a frame that is guaranteed to tear down the peer's channel.
  if (data.size() > kMaxMessageSize) {
    return Error(Error::Code::kParameterInvalid, "Message too large");
  }

  std::vector<uint8_t> frame(sizeof(FrameType) + sizeof(uint32_t) +
                             data.size());
  BigEndianWriter writer(frame);
  writer.Write<uint8_t>(static_cast<uint8_t>(FrameType::kData));
  writer.Write<uint32_t>(static_cast<uint32_t>(data.size()));
  writer.Write(data);

  // Distinguish the two failures: kInsufficientBuffer tells the caller to wait
  // for OnWriteReady() and retry, which would hang forever if the stream is
  // already gone.
  if (!stream_) {
    return Error(Error::Code::kConnectionFailed, "Underlying stream destroyed");
  }
  if (!stream_->Write(frame)) {
    return Error(Error::Code::kInsufficientBuffer, "Write buffer full");
  }

  return Error::None();
}

void DataChannelStream::Close() {
  if (state_ == DataChannel::State::kClosed ||
      state_ == DataChannel::State::kClosing) {
    return;
  }
  state_ = DataChannel::State::kClosing;
  WeakPtr<DataChannelStream> weak_this = weak_factory_.GetWeakPtr();
  if (delegate_) {
    delegate_->OnStateChange(state_);
    if (!weak_this) {
      return;
    }
  }

  if (stream_) {
    stream_->Close();
  }

  state_ = DataChannel::State::kClosed;
  if (delegate_) {
    delegate_->OnStateChange(state_);
    if (!weak_this) {
      return;
    }
  }
  if (session_delegate_) {
    session_delegate_->OnStreamClosed(this);
  }
}

void DataChannelStream::OnRead(WebTransportStream* stream, ByteView data) {
  read_buffer_.insert(read_buffer_.end(), data.begin(), data.end());

  // `read_buffer_` stays bounded because ProcessIncomingData() rejects any
  // frame claiming to be longer than kMaxMessageSize, so at most one partial
  // frame of that size is ever left pending here.
  ProcessIncomingData();
}

void DataChannelStream::OnClose(WebTransportStream* stream) {
  if (state_ == DataChannel::State::kClosed) {
    return;
  }
  state_ = DataChannel::State::kClosed;
  WeakPtr<DataChannelStream> weak_this = weak_factory_.GetWeakPtr();
  if (delegate_) {
    delegate_->OnStateChange(state_);
    if (!weak_this) {
      return;
    }
  }
  if (session_delegate_) {
    session_delegate_->OnStreamClosed(this);
  }
}

void DataChannelStream::OnError(WebTransportStream* stream,
                                const Error& error) {
  Close();
}

void DataChannelStream::OnWriteReady(WebTransportStream* stream) {
  if (delegate_) {
    delegate_->OnWriteReady();
  }
}

void DataChannelStream::OnDestroyed(WebTransportStream* stream) {
  // The transport stream is going away. For incoming channels it is owned by
  // the session, so this is the only notification we get before the pointer
  // becomes invalid. Drop it before doing anything else.
  stream_ = nullptr;

  // Run the normal close path so that the channel and the session observe the
  // closure. Note this may delete `this` via OnStreamClosed().
  OnClose(stream);
}

bool DataChannelStream::SendOpenFrame() {
  OSP_CHECK(label_.size() <= std::numeric_limits<uint16_t>::max());
  std::vector<uint8_t> frame(sizeof(FrameType) + sizeof(uint16_t) +
                             label_.size());
  BigEndianWriter writer(frame);
  writer.Write<uint8_t>(static_cast<uint8_t>(FrameType::kOpen));
  writer.Write<uint16_t>(static_cast<uint16_t>(label_.size()));
  writer.Write(ByteViewFromString(label_));

  if (!stream_ || !stream_->Write(frame)) {
    OSP_LOG_ERROR << "Failed to send open frame";
    // Don't leave the channel stuck in kConnecting: the peer never learns about
    // it, so close it and let the caller/delegate observe the failure. Note
    // this may delete `this` via OnStreamClosed().
    Close();
    return false;
  }

  state_ = DataChannel::State::kOpen;
  if (delegate_) {
    delegate_->OnStateChange(state_);
  }
  return true;
}

void DataChannelStream::ProcessIncomingData() {
  if (parse_state_ == ParseState::kError) {
    read_buffer_.clear();
    read_offset_ = 0;
    return;
  }

  // Reclaim the prefix consumed by previous calls. Compacting once per call
  // instead of once per frame keeps a burst of small frames linear rather than
  // quadratic.
  if (read_offset_ > 0) {
    read_buffer_.erase(read_buffer_.begin(),
                       read_buffer_.begin() + read_offset_);
    read_offset_ = 0;
  }

  // Every step advances `read_offset_` before invoking a delegate callback, so
  // a re-entrant call always observes a consistent cursor.
  while (read_offset_ < read_buffer_.size()) {
    BigEndianReader reader(ByteView(read_buffer_).subspan(read_offset_));

    StepResult result = StepResult::kStop;
    switch (parse_state_) {
      case ParseState::kReadingFrameType:
        result = ProcessFrameType(reader);
        break;
      case ParseState::kReadingLabelLength:
        result = ProcessLabelLength(reader);
        break;
      case ParseState::kReadingLabel:
        result = ProcessLabel(reader);
        break;
      case ParseState::kReadingMessageLength:
        result = ProcessMessageLength(reader);
        break;
      case ParseState::kReadingMessagePayload:
        result = ProcessMessagePayload(reader);
        break;
      case ParseState::kError:
        break;
    }

    // Both kNeedMoreData and kStop end this pass. On kStop `this` may already
    // be destroyed, so no member may be touched.
    if (result != StepResult::kContinue) {
      return;
    }
  }
}

DataChannelStream::StepResult DataChannelStream::ProcessFrameType(
    BigEndianReader& reader) {
  uint8_t type;
  if (!reader.Read(&type)) {
    return StepResult::kNeedMoreData;
  }
  pending_frame_type_ = type;
  if (type == static_cast<uint8_t>(FrameType::kOpen)) {
    parse_state_ = ParseState::kReadingLabelLength;
  } else if (type == static_cast<uint8_t>(FrameType::kData)) {
    parse_state_ = ParseState::kReadingMessageLength;
  } else {
    parse_state_ = ParseState::kError;
    Close();
    return StepResult::kStop;
  }
  read_offset_ += reader.offset();
  return StepResult::kContinue;
}

DataChannelStream::StepResult DataChannelStream::ProcessLabelLength(
    BigEndianReader& reader) {
  uint16_t length;
  if (!reader.Read(&length)) {
    return StepResult::kNeedMoreData;
  }
  pending_label_length_ = length;
  read_offset_ += reader.offset();
  if (pending_label_length_ == 0) {
    return HandleOpenFrame(std::string());
  }
  parse_state_ = ParseState::kReadingLabel;
  return StepResult::kContinue;
}

DataChannelStream::StepResult DataChannelStream::ProcessLabel(
    BigEndianReader& reader) {
  if (reader.remaining() < pending_label_length_) {
    return StepResult::kNeedMoreData;
  }
  std::string label =
      ByteViewToString(reader.remaining_span().first(pending_label_length_));
  reader.Skip(pending_label_length_);
  read_offset_ += reader.offset();
  return HandleOpenFrame(std::move(label));
}

DataChannelStream::StepResult DataChannelStream::ProcessMessageLength(
    BigEndianReader& reader) {
  uint32_t length;
  if (!reader.Read(&length)) {
    return StepResult::kNeedMoreData;
  }
  if (length > kMaxMessageSize) {
    // The peer is either buggy or hostile. Buffering this much would let it
    // drive our memory use arbitrarily, so tear the channel down instead.
    OSP_LOG_ERROR << "Rejecting oversized data frame: " << length
                  << " bytes (max " << kMaxMessageSize << ")";
    parse_state_ = ParseState::kError;
    Close();
    return StepResult::kStop;
  }
  pending_message_length_ = length;
  read_offset_ += reader.offset();
  if (pending_message_length_ == 0) {
    parse_state_ = ParseState::kReadingFrameType;
    return DeliverMessage(ByteView());
  }
  parse_state_ = ParseState::kReadingMessagePayload;
  return StepResult::kContinue;
}

DataChannelStream::StepResult DataChannelStream::ProcessMessagePayload(
    BigEndianReader& reader) {
  if (reader.remaining() < pending_message_length_) {
    return StepResult::kNeedMoreData;
  }
  // Copied out rather than viewed in place: a delegate may append to
  // `read_buffer_` re-entrantly, which would reallocate it mid-callback.
  ByteView view = reader.remaining_span().first(pending_message_length_);
  std::vector<uint8_t> payload(view.begin(), view.end());
  reader.Skip(pending_message_length_);
  parse_state_ = ParseState::kReadingFrameType;
  read_offset_ += reader.offset();
  return DeliverMessage(payload);
}

DataChannelStream::StepResult DataChannelStream::HandleOpenFrame(
    std::string label) {
  parse_state_ = ParseState::kReadingFrameType;

  // A duplicate OPEN on an already-open stream is ignored rather than applied:
  // label() hands out a std::string_view, so reassigning `label_` here would
  // dangle any view the application is still holding.
  if (state_ == DataChannel::State::kOpen) {
    return StepResult::kContinue;
  }

  label_ = std::move(label);
  state_ = DataChannel::State::kOpen;

  WeakPtr<DataChannelStream> weak_this = weak_factory_.GetWeakPtr();
  if (session_delegate_) {
    session_delegate_->OnStreamReady(this);
    if (!weak_this) {
      return StepResult::kStop;
    }
  }
  if (delegate_) {
    delegate_->OnStateChange(state_);
    if (!weak_this) {
      return StepResult::kStop;
    }
  }
  return StepResult::kContinue;
}

DataChannelStream::StepResult DataChannelStream::DeliverMessage(
    ByteView payload) {
  if (!delegate_) {
    return StepResult::kContinue;
  }
  WeakPtr<DataChannelStream> weak_this = weak_factory_.GetWeakPtr();
  delegate_->OnMessage(payload);
  return weak_this ? StepResult::kContinue : StepResult::kStop;
}

}  // namespace openscreen::cast
