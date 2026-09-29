// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STREAMING_IMPL_DATA_CHANNEL_STREAM_H_
#define CAST_STREAMING_IMPL_DATA_CHANNEL_STREAM_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cast/streaming/public/data_channel.h"
#include "platform/api/web_transport.h"
#include "util/big_endian.h"
#include "util/raw_ptr.h"
#include "util/weak_ptr.h"

namespace openscreen::cast {

class DataChannelStream final : public WebTransportStream::Delegate {
 public:
  class Delegate {
   public:
    virtual ~Delegate() = default;
    virtual void OnMessage(ByteView data) = 0;
    virtual void OnStateChange(DataChannel::State state) = 0;
    virtual void OnWriteReady() = 0;
    virtual void OnDestroyed() = 0;
  };

  class SessionDelegate {
   public:
    virtual ~SessionDelegate() = default;
    virtual void OnStreamReady(DataChannelStream* stream) = 0;
    virtual void OnStreamClosed(DataChannelStream* stream) = 0;
  };

  // Upper bound on the payload size of a single data frame. Peers announcing a
  // larger message are treated as a protocol violation: without this bound a
  // remote peer could make us buffer up to 4 GiB by simply lying about the
  // length prefix.
  static constexpr uint32_t kMaxMessageSize = 256 * 1024;

  // For outgoing channels (Sender).
  DataChannelStream(std::unique_ptr<WebTransportStream> stream,
                    std::string label,
                    SessionDelegate* session_delegate);

  // For incoming channels (Receiver).
  DataChannelStream(WebTransportStream* stream,
                    SessionDelegate* session_delegate);

  ~DataChannelStream() override;

  // Subscribes to transport events. This is deliberately separate from the
  // constructor: WebTransportStream::SetDelegate() may synchronously deliver
  // buffered data, which can re-enter the owning session. Callers must only
  // call this once the session fully owns this object.
  void Start();

  void SetDelegate(Delegate* delegate);

  [[nodiscard]] Error Send(ByteView data);
  void Close();

  std::string_view label() const { return label_; }
  DataChannel::State state() const { return state_; }

  WeakPtr<DataChannelStream> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

  // WebTransportStream::Delegate overrides.
  void OnRead(WebTransportStream* stream, ByteView data) override;
  void OnClose(WebTransportStream* stream) override;
  void OnError(WebTransportStream* stream, const Error& error) override;
  void OnWriteReady(WebTransportStream* stream) override;
  void OnDestroyed(WebTransportStream* stream) override;

  // Sends the OPEN frame and transitions to kOpen. On failure the channel is
  // closed, which may destroy `this` via SessionDelegate::OnStreamClosed(), and
  // false is returned; the caller must not touch `this` afterwards.
  [[nodiscard]] bool SendOpenFrame();

 private:
  // Outcome of a single parser step.
  enum class StepResult {
    // A frame component was consumed; keep parsing.
    kContinue,
    // Not enough buffered bytes to make progress; wait for more data.
    kNeedMoreData,
    // Parsing must stop immediately: the channel was closed or `this` was
    // destroyed by a delegate callback.
    kStop,
  };

  void ProcessIncomingData();

  // Per-state parser steps. Each consumes from `reader` and, on success,
  // advances `read_offset_` before invoking any delegate callback.
  StepResult ProcessFrameType(BigEndianReader& reader);
  StepResult ProcessLabelLength(BigEndianReader& reader);
  StepResult ProcessLabel(BigEndianReader& reader);
  StepResult ProcessMessageLength(BigEndianReader& reader);
  StepResult ProcessMessagePayload(BigEndianReader& reader);

  // Handles a fully parsed OPEN frame carrying `label`.
  StepResult HandleOpenFrame(std::string label);

  // Delivers a fully parsed DATA frame payload to the delegate.
  StepResult DeliverMessage(ByteView payload);

  enum class ParseState {
    kReadingFrameType,
    kReadingLabelLength,
    kReadingLabel,
    kReadingMessageLength,
    kReadingMessagePayload,
    kError
  };

  enum class FrameType : uint8_t {
    kOpen = 0x01,
    kData = 0x02,
  };

  std::unique_ptr<WebTransportStream> owned_stream_;
  raw_ptr<WebTransportStream> stream_;
  std::string label_;
  DataChannel::State state_ = DataChannel::State::kConnecting;
  raw_ptr<Delegate> delegate_;
  raw_ptr<SessionDelegate> session_delegate_;

  ParseState parse_state_ = ParseState::kReadingFrameType;
  uint8_t pending_frame_type_ = 0;
  uint16_t pending_label_length_ = 0;
  uint32_t pending_message_length_ = 0;
  std::vector<uint8_t> read_buffer_;
  // Index of the first unconsumed byte in `read_buffer_`.
  size_t read_offset_ = 0;

  WeakPtrFactory<DataChannelStream> weak_factory_{this};
};

}  // namespace openscreen::cast

#endif  // CAST_STREAMING_IMPL_DATA_CHANNEL_STREAM_H_
