// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cast/streaming/impl/data_channel_impl.h"
#include "cast/streaming/impl/data_channel_session.h"
#include "cast/streaming/impl/data_channel_stream.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "platform/api/web_transport.h"

namespace openscreen::cast {
namespace {

using ::testing::_;
using ::testing::NiceMock;
using ::testing::StrictMock;

class FakeWebTransportStream : public WebTransportStream {
 public:
  explicit FakeWebTransportStream(FakeWebTransportStream* peer = nullptr)
      : peer_(peer) {}

  // Mirrors QuicheWebTransportStream: the delegate is unset and notified
  // before the stream goes away.
  ~FakeWebTransportStream() override {
    if (peer_) {
      peer_->SetPeer(nullptr);
    }
    if (Delegate* delegate = delegate_) {
      delegate_ = nullptr;
      delegate->OnDestroyed(this);
    }
  }

  void SetPeer(FakeWebTransportStream* peer) { peer_ = peer; }

  void SetDelegate(Delegate* delegate) override {
    delegate_ = delegate;
    FlushIncomingBytes();
  }

  bool Write(ByteView data) override {
    if (write_blocked_) {
      return false;
    }
    std::vector<uint8_t> bytes(data.begin(), data.end());
    if (peer_) {
      peer_->BufferIncomingBytes(std::move(bytes));
    }
    return true;
  }

  void Close() override {
    if (peer_) {
      peer_->NotifyClose();
    }
  }

  void SetWriteBlocked(bool blocked) {
    write_blocked_ = blocked;
    if (!write_blocked_ && delegate_) {
      delegate_->OnWriteReady(this);
    }
  }

  void BufferIncomingBytes(std::vector<uint8_t> bytes) {
    incoming_buffer_.insert(incoming_buffer_.end(), bytes.begin(), bytes.end());
    FlushIncomingBytes();
  }

  void FlushIncomingBytes() {
    if (delegate_ && !incoming_buffer_.empty()) {
      std::vector<uint8_t> data = std::move(incoming_buffer_);
      incoming_buffer_.clear();
      delegate_->OnRead(this, ByteView(data.data(), data.size()));
    }
  }

  void NotifyClose() {
    if (delegate_) {
      delegate_->OnClose(this);
    }
  }

 private:
  FakeWebTransportStream* peer_;
  Delegate* delegate_ = nullptr;
  bool write_blocked_ = false;
  std::vector<uint8_t> incoming_buffer_;
};

class MockWebTransportSession : public WebTransportSession {
 public:
  MOCK_METHOD(void, SetDelegate, (Delegate * delegate), (override));
  MOCK_METHOD(ErrorOr<std::unique_ptr<WebTransportStream>>,
              CreateOutgoingStream,
              (),
              (override));
  MOCK_METHOD(void, Close, (const Error& error), (override));
};

class MockSessionVisitor : public DataChannelSession::Visitor {
 public:
  MOCK_METHOD(void,
              OnNewInputChannel,
              (std::unique_ptr<DataChannel> channel),
              (override));
  MOCK_METHOD(void, OnConnectionEstablished, (), (override));
  MOCK_METHOD(void, OnConnectionClosed, (), (override));
};

class MockChannelDelegate : public DataChannel::Delegate {
 public:
  MOCK_METHOD(void, OnMessage, (ByteView data), (override));
  MOCK_METHOD(void, OnStateChange, (DataChannel::State state), (override));
  MOCK_METHOD(void, OnWriteReady, (), (override));
};

class DataChannelTest : public ::testing::Test {
 protected:
  DataChannelTest() = default;

  void SetUp() override {
    auto client_session = std::make_unique<NiceMock<MockWebTransportSession>>();
    client_session_raw_ = client_session.get();
    auto server_session = std::make_unique<NiceMock<MockWebTransportSession>>();
    server_session_raw_ = server_session.get();

    client_data_session_ = std::make_unique<DataChannelSession>(
        std::move(client_session), &client_session_visitor_);
    server_data_session_ = std::make_unique<DataChannelSession>(
        std::move(server_session), &server_session_visitor_);
  }

  NiceMock<MockWebTransportSession>* client_session_raw_;
  NiceMock<MockWebTransportSession>* server_session_raw_;

  MockSessionVisitor client_session_visitor_;
  MockSessionVisitor server_session_visitor_;

  std::unique_ptr<DataChannelSession> client_data_session_;
  std::unique_ptr<DataChannelSession> server_data_session_;
};

TEST_F(DataChannelTest, CreateDataChannel_ExchangeMessages_Succeeds) {
  StrictMock<MockChannelDelegate> client_delegate;
  StrictMock<MockChannelDelegate> server_delegate;
  std::unique_ptr<DataChannel> server_channel;

  auto client_stream = std::make_unique<FakeWebTransportStream>();
  auto server_stream = std::make_unique<FakeWebTransportStream>();
  client_stream->SetPeer(server_stream.get());
  server_stream->SetPeer(client_stream.get());

  EXPECT_CALL(*client_session_raw_, CreateOutgoingStream()).WillOnce([&]() {
    return ErrorOr<std::unique_ptr<WebTransportStream>>(
        std::move(client_stream));
  });

  // 1. Expect Server to receive new incoming channel upon open frame.
  EXPECT_CALL(server_session_visitor_, OnNewInputChannel(_))
      .WillOnce([&](std::unique_ptr<DataChannel> channel) {
        server_channel = std::move(channel);
      });

  // 2. Client creates outgoing data channel.
  auto client_channel =
      client_data_session_->CreateOutgoingDataChannel("test-label");
  ASSERT_NE(client_channel, nullptr);
  EXPECT_EQ(client_channel->label(), "test-label");

  EXPECT_CALL(client_delegate, OnStateChange(DataChannel::State::kOpen));
  client_channel->SetDelegate(&client_delegate);

  server_data_session_->OnIncomingStream(server_stream.get());

  ASSERT_NE(server_channel, nullptr);
  EXPECT_EQ(server_channel->label(), "test-label");

  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kOpen));
  server_channel->SetDelegate(&server_delegate);

  EXPECT_EQ(client_channel->state(), DataChannel::State::kOpen);
  EXPECT_EQ(server_channel->state(), DataChannel::State::kOpen);

  // 3. Send message from Client to Server.
  EXPECT_CALL(server_delegate, OnMessage(_)).WillOnce([](ByteView data) {
    std::string msg(reinterpret_cast<const char*>(data.data()), data.size());
    EXPECT_EQ(msg, "Hello Server!");
  });

  std::string msg = "Hello Server!";
  Error error = client_channel->Send(
      ByteView(reinterpret_cast<const uint8_t*>(msg.data()), msg.size()));
  EXPECT_TRUE(error.ok());

  // 4. Send message from Server to Client.
  EXPECT_CALL(client_delegate, OnMessage(_)).WillOnce([](ByteView data) {
    std::string msg(reinterpret_cast<const char*>(data.data()), data.size());
    EXPECT_EQ(msg, "Hello Client!");
  });

  std::string response = "Hello Client!";
  error = server_channel->Send(ByteView(
      reinterpret_cast<const uint8_t*>(response.data()), response.size()));
  EXPECT_TRUE(error.ok());

  // 5. Clean teardown.
  EXPECT_CALL(client_delegate, OnStateChange(DataChannel::State::kClosing));
  EXPECT_CALL(client_delegate, OnStateChange(DataChannel::State::kClosed));
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosed));
  client_channel->Close();
}

TEST_F(DataChannelTest, ReceiveMessage_Fragmented_SuccessfullyReassembled) {
  StrictMock<MockChannelDelegate> server_delegate;
  std::unique_ptr<DataChannel> server_channel;

  auto client_stream = std::make_unique<FakeWebTransportStream>();
  auto server_stream = std::make_unique<FakeWebTransportStream>();
  client_stream->SetPeer(server_stream.get());
  server_stream->SetPeer(client_stream.get());

  auto* client_stream_ptr = client_stream.get();

  EXPECT_CALL(server_session_visitor_, OnNewInputChannel(_))
      .WillOnce([&](std::unique_ptr<DataChannel> channel) {
        server_channel = std::move(channel);
      });

  server_data_session_->OnIncomingStream(server_stream.get());

  // Send Open Frame: Type (0x01) + Length (0x0005) + "label"
  client_stream_ptr->Write(
      ByteView(reinterpret_cast<const uint8_t*>("\x01\x00\x05label"), 8));

  ASSERT_NE(server_channel, nullptr);
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kOpen));
  server_channel->SetDelegate(&server_delegate);

  ASSERT_EQ(server_channel->label(), "label");
  ASSERT_EQ(server_channel->state(), DataChannel::State::kOpen);

  // Send Fragmented Message:
  // Total Frame: Type (0x02) + Length (0x0000000C) + "Hello World!"
  EXPECT_CALL(server_delegate, OnMessage(_)).WillOnce([](ByteView data) {
    std::string msg(reinterpret_cast<const char*>(data.data()), data.size());
    EXPECT_EQ(msg, "Hello World!");
  });

  // Part 1: Type + partial length
  client_stream_ptr->Write(
      ByteView(reinterpret_cast<const uint8_t*>("\x02\x00\x00"), 3));
  // Part 2: Rest of length + partial payload
  client_stream_ptr->Write(
      ByteView(reinterpret_cast<const uint8_t*>("\x00\x0CHello"), 7));
  // Part 3: Rest of payload
  client_stream_ptr->Write(
      ByteView(reinterpret_cast<const uint8_t*>(" World!"), 7));

  // Clean teardown.
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosing));
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosed));
  server_channel->Close();
}

TEST_F(DataChannelTest, OnConnectionClosed_AbruptClose_ChannelsClosed) {
  StrictMock<MockChannelDelegate> client_delegate;
  StrictMock<MockChannelDelegate> server_delegate;
  std::unique_ptr<DataChannel> server_channel;

  auto client_stream = std::make_unique<FakeWebTransportStream>();
  auto server_stream = std::make_unique<FakeWebTransportStream>();
  client_stream->SetPeer(server_stream.get());
  server_stream->SetPeer(client_stream.get());

  EXPECT_CALL(*client_session_raw_, CreateOutgoingStream()).WillOnce([&]() {
    return ErrorOr<std::unique_ptr<WebTransportStream>>(
        std::move(client_stream));
  });

  // 1. Expect incoming channel
  EXPECT_CALL(server_session_visitor_, OnNewInputChannel(_))
      .WillOnce([&](std::unique_ptr<DataChannel> channel) {
        server_channel = std::move(channel);
      });

  auto client_channel =
      client_data_session_->CreateOutgoingDataChannel("test-label");
  ASSERT_NE(client_channel, nullptr);

  EXPECT_CALL(client_delegate, OnStateChange(DataChannel::State::kOpen));
  client_channel->SetDelegate(&client_delegate);

  server_data_session_->OnIncomingStream(server_stream.get());

  ASSERT_NE(server_channel, nullptr);
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kOpen));
  server_channel->SetDelegate(&server_delegate);

  // Close session.
  EXPECT_CALL(client_delegate, OnStateChange(DataChannel::State::kClosed));
  client_data_session_->OnSessionClosed(client_session_raw_,
                                        Error(Error::Code::kNone, "closed"));

  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosed));
  server_data_session_->OnSessionClosed(server_session_raw_,
                                        Error(Error::Code::kNone, "closed"));

  EXPECT_EQ(client_channel->state(), DataChannel::State::kClosed);
  EXPECT_EQ(server_channel->state(), DataChannel::State::kClosed);
}

TEST_F(DataChannelTest, OnCanWrite_WriteUnblocked_OnWriteReadyCalled) {
  StrictMock<MockChannelDelegate> client_delegate;

  auto client_stream = std::make_unique<FakeWebTransportStream>();
  auto* client_stream_ptr = client_stream.get();

  EXPECT_CALL(*client_session_raw_, CreateOutgoingStream()).WillOnce([&]() {
    return ErrorOr<std::unique_ptr<WebTransportStream>>(
        std::move(client_stream));
  });

  auto client_channel =
      client_data_session_->CreateOutgoingDataChannel("test-label");
  ASSERT_NE(client_channel, nullptr);

  EXPECT_CALL(client_delegate, OnStateChange(DataChannel::State::kOpen));
  client_channel->SetDelegate(&client_delegate);

  // 1. Block the write.
  client_stream_ptr->SetWriteBlocked(true);

  std::string msg = "blocked msg";
  Error error = client_channel->Send(
      ByteView(reinterpret_cast<const uint8_t*>(msg.data()), msg.size()));
  EXPECT_FALSE(error.ok());
  EXPECT_EQ(error.code(), Error::Code::kInsufficientBuffer);

  // 2. Unblock the write. Expect delegate OnWriteReady to be called.
  EXPECT_CALL(client_delegate, OnWriteReady());
  client_stream_ptr->SetWriteBlocked(false);

  // Clean teardown.
  EXPECT_CALL(client_delegate, OnStateChange(DataChannel::State::kClosing));
  EXPECT_CALL(client_delegate, OnStateChange(DataChannel::State::kClosed));
  client_channel->Close();
}

TEST_F(DataChannelTest,
       CreateOutgoingDataChannel_OpenFrameWriteFails_ReturnsNullAndReleases) {
  auto client_stream = std::make_unique<FakeWebTransportStream>();
  client_stream->SetWriteBlocked(true);

  EXPECT_CALL(*client_session_raw_, CreateOutgoingStream()).WillOnce([&]() {
    return ErrorOr<std::unique_ptr<WebTransportStream>>(
        std::move(client_stream));
  });

  auto client_channel =
      client_data_session_->CreateOutgoingDataChannel("test-label");
  EXPECT_EQ(client_channel, nullptr);
  EXPECT_EQ(client_data_session_->GetActiveStreamCountForTesting(), 0u);
}

TEST_F(DataChannelTest, ReceiveMessage_InvalidFrameType_ClosesChannel) {
  StrictMock<MockChannelDelegate> server_delegate;
  std::unique_ptr<DataChannel> server_channel;

  auto client_stream = std::make_unique<FakeWebTransportStream>();
  auto server_stream = std::make_unique<FakeWebTransportStream>();
  client_stream->SetPeer(server_stream.get());
  server_stream->SetPeer(client_stream.get());

  auto* client_stream_ptr = client_stream.get();

  EXPECT_CALL(server_session_visitor_, OnNewInputChannel(_))
      .WillOnce([&](std::unique_ptr<DataChannel> channel) {
        server_channel = std::move(channel);
      });

  server_data_session_->OnIncomingStream(server_stream.get());

  // Send Open Frame
  client_stream_ptr->Write(
      ByteView(reinterpret_cast<const uint8_t*>("\x01\x00\x05label"), 8));

  ASSERT_NE(server_channel, nullptr);
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kOpen));
  server_channel->SetDelegate(&server_delegate);

  // Send invalid frame type 0x03.
  // Expect channel to close.
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosing));
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosed));
  client_stream_ptr->Write(
      ByteView(reinterpret_cast<const uint8_t*>("\x03\x00\x05Hello"), 8));

  EXPECT_EQ(server_channel->state(), DataChannel::State::kClosed);
}

TEST_F(DataChannelTest,
       ReceiveMessage_ZeroLengthMessage_DeliveredSuccessfully) {
  StrictMock<MockChannelDelegate> server_delegate;
  std::unique_ptr<DataChannel> server_channel;

  auto client_stream = std::make_unique<FakeWebTransportStream>();
  auto server_stream = std::make_unique<FakeWebTransportStream>();
  client_stream->SetPeer(server_stream.get());
  server_stream->SetPeer(client_stream.get());

  auto* client_stream_ptr = client_stream.get();

  EXPECT_CALL(server_session_visitor_, OnNewInputChannel(_))
      .WillOnce([&](std::unique_ptr<DataChannel> channel) {
        server_channel = std::move(channel);
      });

  server_data_session_->OnIncomingStream(server_stream.get());

  // Send Open Frame
  client_stream_ptr->Write(
      ByteView(reinterpret_cast<const uint8_t*>("\x01\x00\x05label"), 8));

  ASSERT_NE(server_channel, nullptr);
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kOpen));
  server_channel->SetDelegate(&server_delegate);

  // Send message type 0x02 with length 0.
  EXPECT_CALL(server_delegate, OnMessage(_)).WillOnce([](ByteView data) {
    EXPECT_EQ(data.size(), 0u);
  });

  client_stream_ptr->Write(
      ByteView(reinterpret_cast<const uint8_t*>("\x02\x00\x00\x00\x00"), 5));

  // Clean teardown.
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosing));
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosed));
  server_channel->Close();
}

TEST_F(DataChannelTest, ReceiveMessage_DuplicateOpenFrame_IgnoredGracefully) {
  StrictMock<MockChannelDelegate> server_delegate;
  std::unique_ptr<DataChannel> server_channel;

  auto client_stream = std::make_unique<FakeWebTransportStream>();
  auto server_stream = std::make_unique<FakeWebTransportStream>();
  client_stream->SetPeer(server_stream.get());
  server_stream->SetPeer(client_stream.get());

  auto* client_stream_ptr = client_stream.get();

  EXPECT_CALL(server_session_visitor_, OnNewInputChannel(_))
      .WillOnce([&](std::unique_ptr<DataChannel> channel) {
        server_channel = std::move(channel);
      });

  server_data_session_->OnIncomingStream(server_stream.get());

  // Send Open Frame
  client_stream_ptr->Write(
      ByteView(reinterpret_cast<const uint8_t*>("\x01\x00\x05label"), 8));

  ASSERT_NE(server_channel, nullptr);
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kOpen));
  server_channel->SetDelegate(&server_delegate);

  // Send duplicate DATA_CHANNEL_OPEN frame type 0x01.
  // Expect no state changes and no crash.
  client_stream_ptr->Write(
      ByteView(reinterpret_cast<const uint8_t*>("\x01\x00\x05label"), 8));

  // Clean teardown.
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosing));
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosed));
  server_channel->Close();
}

TEST_F(DataChannelTest,
       ReceiveMessage_DelegateClosesChannelInsideOnMessage_DoesNotCrash) {
  StrictMock<MockChannelDelegate> server_delegate;
  std::unique_ptr<DataChannel> server_channel;

  auto client_stream = std::make_unique<FakeWebTransportStream>();
  auto server_stream = std::make_unique<FakeWebTransportStream>();
  client_stream->SetPeer(server_stream.get());
  server_stream->SetPeer(client_stream.get());

  auto* client_stream_ptr = client_stream.get();

  EXPECT_CALL(server_session_visitor_, OnNewInputChannel(_))
      .WillOnce([&](std::unique_ptr<DataChannel> channel) {
        server_channel = std::move(channel);
      });

  server_data_session_->OnIncomingStream(server_stream.get());

  // Send Open Frame
  client_stream_ptr->Write(
      ByteView(reinterpret_cast<const uint8_t*>("\x01\x00\x05label"), 8));

  ASSERT_NE(server_channel, nullptr);
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kOpen));
  server_channel->SetDelegate(&server_delegate);

  // Send two messages back-to-back in a single write.
  // When the first message arrives, the delegate closes and resets the channel.
  // The subsequent message processing must not crash or trigger use-after-free.
  EXPECT_CALL(server_delegate, OnMessage(_)).WillOnce([&](ByteView data) {
    EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosing));
    EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosed));
    server_channel->Close();
    server_channel.reset();
  });

  std::string two_messages =
      std::string("\x02\x00\x00\x00\x04msg1\x02\x00\x00\x00\x04msg2", 18);
  client_stream_ptr->Write(
      ByteView(reinterpret_cast<const uint8_t*>(two_messages.data()),
               two_messages.size()));
}

// Incoming streams are owned by the WebTransport session, not by the
// DataChannelStream. When the session tears one down, the channel must be
// notified via Delegate::OnDestroyed() so that it drops its pointer; otherwise
// any later Send() or teardown dereferences freed memory.
TEST_F(DataChannelTest,
       TransportStreamDestroyed_ChannelStillOpen_ClosesWithoutUseAfterFree) {
  StrictMock<MockChannelDelegate> server_delegate;
  std::unique_ptr<DataChannel> server_channel;

  auto client_stream = std::make_unique<FakeWebTransportStream>();
  auto server_stream = std::make_unique<FakeWebTransportStream>();
  client_stream->SetPeer(server_stream.get());
  server_stream->SetPeer(client_stream.get());

  auto* client_stream_ptr = client_stream.get();

  EXPECT_CALL(server_session_visitor_, OnNewInputChannel(_))
      .WillOnce([&](std::unique_ptr<DataChannel> channel) {
        server_channel = std::move(channel);
      });

  server_data_session_->OnIncomingStream(server_stream.get());

  // Send Open Frame: Type (0x01) + Length (0x0005) + "label"
  client_stream_ptr->Write(
      ByteView(reinterpret_cast<const uint8_t*>("\x01\x00\x05label"), 8));

  ASSERT_NE(server_channel, nullptr);
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kOpen));
  server_channel->SetDelegate(&server_delegate);
  ASSERT_EQ(server_channel->state(), DataChannel::State::kOpen);

  // The session destroys the transport stream out from under the channel.
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosed));
  server_stream.reset();

  // The channel observed the closure and no longer references the dead stream.
  EXPECT_EQ(server_channel->state(), DataChannel::State::kClosed);
  // A destroyed stream can never deliver OnWriteReady(), so this must be a
  // permanent failure rather than a "retry later" kInsufficientBuffer.
  EXPECT_EQ(server_channel->Send(ByteView()).code(),
            Error::Code::kConnectionFailed);

  // Destroying the channel afterwards must not touch the freed stream.
  server_channel.reset();
}

// Subscribing to the transport can synchronously drain already-buffered bytes,
// running the whole OPEN frame -> OnStreamReady -> OnNewInputChannel chain
// inline. If the visitor closes the channel right there, OnStreamClosed() must
// still be able to find the stream, which is only true if the session owns it
// before the subscription happens.
TEST_F(DataChannelTest,
       OnIncomingStream_VisitorClosesChannelDuringNotification_DoesNotLeak) {
  auto client_stream = std::make_unique<FakeWebTransportStream>();
  auto server_stream = std::make_unique<FakeWebTransportStream>();
  client_stream->SetPeer(server_stream.get());
  server_stream->SetPeer(client_stream.get());

  // Buffer the Open Frame before the session ever sees the stream, so that it
  // is delivered synchronously from within OnIncomingStream().
  client_stream->Write(
      ByteView(reinterpret_cast<const uint8_t*>("\x01\x00\x05label"), 8));

  EXPECT_CALL(server_session_visitor_, OnNewInputChannel(_))
      .WillOnce([](std::unique_ptr<DataChannel> channel) {
        // Drop the channel immediately, re-entering OnStreamClosed().
        channel.reset();
      });

  server_data_session_->OnIncomingStream(server_stream.get());

  EXPECT_EQ(server_data_session_->GetActiveStreamCountForTesting(), 0u);
}

// The message length is read straight off the wire as a uint32_t. Trusting it
// would let a peer make us reserve up to 4 GiB, so oversized frames must be
// treated as a protocol error.
TEST_F(DataChannelTest, ReceiveMessage_LengthExceedsMaximum_ClosesChannel) {
  StrictMock<MockChannelDelegate> server_delegate;
  std::unique_ptr<DataChannel> server_channel;

  auto client_stream = std::make_unique<FakeWebTransportStream>();
  auto server_stream = std::make_unique<FakeWebTransportStream>();
  client_stream->SetPeer(server_stream.get());
  server_stream->SetPeer(client_stream.get());

  auto* client_stream_ptr = client_stream.get();

  EXPECT_CALL(server_session_visitor_, OnNewInputChannel(_))
      .WillOnce([&](std::unique_ptr<DataChannel> channel) {
        server_channel = std::move(channel);
      });

  server_data_session_->OnIncomingStream(server_stream.get());

  client_stream_ptr->Write(
      ByteView(reinterpret_cast<const uint8_t*>("\x01\x00\x05label"), 8));

  ASSERT_NE(server_channel, nullptr);
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kOpen));
  server_channel->SetDelegate(&server_delegate);

  // Data frame announcing a 0xFFFFFFFF byte payload. OnMessage must never fire.
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosing));
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosed));
  client_stream_ptr->Write(
      ByteView(reinterpret_cast<const uint8_t*>("\x02\xFF\xFF\xFF\xFF"), 5));

  EXPECT_EQ(server_channel->state(), DataChannel::State::kClosed);
}

// A payload exactly at the limit is still legal and must be delivered.
TEST_F(DataChannelTest, ReceiveMessage_LengthAtMaximum_DeliversMessage) {
  StrictMock<MockChannelDelegate> server_delegate;
  std::unique_ptr<DataChannel> server_channel;

  auto client_stream = std::make_unique<FakeWebTransportStream>();
  auto server_stream = std::make_unique<FakeWebTransportStream>();
  client_stream->SetPeer(server_stream.get());
  server_stream->SetPeer(client_stream.get());

  auto* client_stream_ptr = client_stream.get();

  EXPECT_CALL(server_session_visitor_, OnNewInputChannel(_))
      .WillOnce([&](std::unique_ptr<DataChannel> channel) {
        server_channel = std::move(channel);
      });

  server_data_session_->OnIncomingStream(server_stream.get());

  client_stream_ptr->Write(
      ByteView(reinterpret_cast<const uint8_t*>("\x01\x00\x05label"), 8));

  ASSERT_NE(server_channel, nullptr);
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kOpen));
  server_channel->SetDelegate(&server_delegate);

  constexpr uint32_t kSize = DataChannelStream::kMaxMessageSize;
  std::vector<uint8_t> frame;
  frame.push_back(0x02);
  frame.push_back(static_cast<uint8_t>(kSize >> 24));
  frame.push_back(static_cast<uint8_t>(kSize >> 16));
  frame.push_back(static_cast<uint8_t>(kSize >> 8));
  frame.push_back(static_cast<uint8_t>(kSize));
  frame.insert(frame.end(), kSize, 0xAB);

  EXPECT_CALL(server_delegate, OnMessage(_)).WillOnce([&](ByteView data) {
    EXPECT_EQ(data.size(), kSize);
  });
  client_stream_ptr->Write(ByteView(frame.data(), frame.size()));

  EXPECT_EQ(server_channel->state(), DataChannel::State::kOpen);

  // `server_stream` is destroyed before `server_channel` at end of scope, which
  // closes the channel.
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosed));
}

// label() hands out a std::string_view into `label_`. A second OPEN frame must
// therefore not reassign it, or any view the application still holds dangles.
TEST_F(DataChannelTest,
       ReceiveMessage_DuplicateOpenFrameWithNewLabel_LabelUnchanged) {
  StrictMock<MockChannelDelegate> server_delegate;
  std::unique_ptr<DataChannel> server_channel;

  auto client_stream = std::make_unique<FakeWebTransportStream>();
  auto server_stream = std::make_unique<FakeWebTransportStream>();
  client_stream->SetPeer(server_stream.get());
  server_stream->SetPeer(client_stream.get());

  auto* client_stream_ptr = client_stream.get();

  EXPECT_CALL(server_session_visitor_, OnNewInputChannel(_))
      .WillOnce([&](std::unique_ptr<DataChannel> channel) {
        server_channel = std::move(channel);
      });

  server_data_session_->OnIncomingStream(server_stream.get());

  client_stream_ptr->Write(
      ByteView(reinterpret_cast<const uint8_t*>("\x01\x00\x05label"), 8));

  ASSERT_NE(server_channel, nullptr);
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kOpen));
  server_channel->SetDelegate(&server_delegate);
  ASSERT_EQ(server_channel->label(), "label");

  // Second OPEN frame carrying a different label.
  client_stream_ptr->Write(
      ByteView(reinterpret_cast<const uint8_t*>("\x01\x00\x05other"), 8));

  EXPECT_EQ(server_channel->label(), "label");

  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosing));
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosed));
  server_channel->Close();
}

// A peer must not be able to exhaust memory by opening streams indefinitely.
TEST_F(DataChannelTest, OnIncomingStream_ExceedsChannelLimit_StreamRejected) {
  constexpr size_t kLimit = DataChannelSession::kMaxConcurrentChannels;

  // The visitor is only notified once a channel opens, which needs an OPEN
  // frame; these bare streams stay in kConnecting, so nothing is notified.
  std::vector<std::unique_ptr<FakeWebTransportStream>> streams;
  for (size_t i = 0; i < kLimit; ++i) {
    streams.push_back(std::make_unique<FakeWebTransportStream>());
    server_data_session_->OnIncomingStream(streams.back().get());
  }
  ASSERT_EQ(server_data_session_->GetActiveStreamCountForTesting(), kLimit);

  auto overflow_stream = std::make_unique<FakeWebTransportStream>();
  server_data_session_->OnIncomingStream(overflow_stream.get());

  EXPECT_EQ(server_data_session_->GetActiveStreamCountForTesting(), kLimit);
}

// The sender enforces the same bound as the receiver, so we never emit a frame
// that is guaranteed to tear down the peer's channel.
TEST_F(DataChannelTest, Send_MessageExceedsMaximum_ReturnsParameterInvalid) {
  StrictMock<MockChannelDelegate> server_delegate;
  std::unique_ptr<DataChannel> server_channel;

  auto client_stream = std::make_unique<FakeWebTransportStream>();
  auto server_stream = std::make_unique<FakeWebTransportStream>();
  client_stream->SetPeer(server_stream.get());
  server_stream->SetPeer(client_stream.get());

  auto* client_stream_ptr = client_stream.get();

  EXPECT_CALL(server_session_visitor_, OnNewInputChannel(_))
      .WillOnce([&](std::unique_ptr<DataChannel> channel) {
        server_channel = std::move(channel);
      });

  server_data_session_->OnIncomingStream(server_stream.get());

  client_stream_ptr->Write(
      ByteView(reinterpret_cast<const uint8_t*>("\x01\x00\x05label"), 8));

  ASSERT_NE(server_channel, nullptr);
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kOpen));
  server_channel->SetDelegate(&server_delegate);

  std::vector<uint8_t> too_big(DataChannelStream::kMaxMessageSize + 1, 0xAB);
  EXPECT_EQ(
      server_channel->Send(ByteView(too_big.data(), too_big.size())).code(),
      Error::Code::kParameterInvalid);

  // The channel is still usable.
  EXPECT_EQ(server_channel->state(), DataChannel::State::kOpen);

  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosing));
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosed));
  server_channel->Close();
}

// Applications are told (see Visitor::OnNewInputChannel) to attach their
// delegate synchronously. Doing so means SetDelegate() reports kOpen, and the
// stream then reports it again as it unwinds out of OnStreamReady(). The
// application must only observe the transition once. Found by running the
// standalone sender/receiver end-to-end.
TEST_F(DataChannelTest,
       OnNewInputChannel_DelegateAttachedDuringCallback_StateReportedOnce) {
  StrictMock<MockChannelDelegate> server_delegate;
  std::unique_ptr<DataChannel> server_channel;

  auto client_stream = std::make_unique<FakeWebTransportStream>();
  auto server_stream = std::make_unique<FakeWebTransportStream>();
  client_stream->SetPeer(server_stream.get());
  server_stream->SetPeer(client_stream.get());

  auto* client_stream_ptr = client_stream.get();

  server_data_session_->OnIncomingStream(server_stream.get());

  // Exactly one kOpen, no matter how many times the stream reports it.
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kOpen))
      .Times(1);
  EXPECT_CALL(server_session_visitor_, OnNewInputChannel(_))
      .WillOnce([&](std::unique_ptr<DataChannel> channel) {
        // This is what MirroringApplication does.
        channel->SetDelegate(&server_delegate);
        server_channel = std::move(channel);
      });

  client_stream_ptr->Write(
      ByteView(reinterpret_cast<const uint8_t*>("\x01\x00\x05label"), 8));

  ASSERT_NE(server_channel, nullptr);
  EXPECT_EQ(server_channel->state(), DataChannel::State::kOpen);

  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosing));
  EXPECT_CALL(server_delegate, OnStateChange(DataChannel::State::kClosed));
  server_channel->Close();
}

}  // namespace
}  // namespace openscreen::cast
