// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <format>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "platform/api/web_transport.h"
#include "platform/base/ip_address.h"
#include "platform/impl/platform_client_posix.h"
#include "testing/util/task_util.h"

namespace openscreen {
namespace {

using ::testing::_;

constexpr auto kTestTimeout = std::chrono::seconds(5);

class TestStreamDelegate : public WebTransportStream::Delegate {
 public:
  explicit TestStreamDelegate(std::promise<std::string> read_promise)
      : read_promise_(std::move(read_promise)) {}

  void OnRead(WebTransportStream* stream, ByteView data) override {
    received_data_.insert(received_data_.end(), data.begin(), data.end());
    std::string msg(received_data_.begin(), received_data_.end());
    if (!msg.empty() && read_promise_) {
      read_promise_->set_value(std::move(msg));
      read_promise_.reset();
    }
  }

  void OnClose(WebTransportStream* stream) override {}
  void OnError(WebTransportStream* stream, const Error& error) override {}

  void OnDestroyed(WebTransportStream* stream) override {
    destroyed_count_++;
    destroyed_stream_ = stream;
  }

  int destroyed_count() const { return destroyed_count_; }
  const WebTransportStream* destroyed_stream() const {
    return destroyed_stream_;
  }

 private:
  std::optional<std::promise<std::string>> read_promise_;
  std::vector<uint8_t> received_data_;
  int destroyed_count_ = 0;
  const WebTransportStream* destroyed_stream_ = nullptr;
};

class TestSessionDelegate : public WebTransportSession::Delegate {
 public:
  TestSessionDelegate() = default;

  void SetIncomingStreamPromise(std::promise<WebTransportStream*> promise) {
    incoming_stream_promise_ = std::move(promise);
  }

  void SetSessionClosedPromise(std::promise<Error> promise) {
    session_closed_promise_ = std::move(promise);
  }

  void OnIncomingStream(WebTransportStream* stream) override {
    incoming_stream_promise_.set_value(stream);
  }

  void OnSessionReady(WebTransportSession* session) override {}

  void OnSessionClosed(WebTransportSession* session,
                       const Error& error) override {
    session_closed_promise_.set_value(error);
  }

 private:
  std::promise<WebTransportStream*> incoming_stream_promise_;
  std::promise<Error> session_closed_promise_;
};

class TestServerDelegate : public WebTransportServer::Delegate {
 public:
  explicit TestServerDelegate(
      std::promise<std::unique_ptr<WebTransportSession>> session_promise)
      : session_promise_(std::move(session_promise)) {}

  void OnSessionCreated(std::unique_ptr<WebTransportSession> session) override {
    session->SetDelegate(&session_delegate_);
    session_promise_.set_value(std::move(session));
  }

  void OnServerClosed() override {}

  TestSessionDelegate& session_delegate() { return session_delegate_; }

 private:
  std::promise<std::unique_ptr<WebTransportSession>> session_promise_;
  TestSessionDelegate session_delegate_;
};

class QuicheWebTransportTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Start the POSIX platform client which starts the network event loop
    // thread.
    PlatformClientPosix::Create(std::chrono::milliseconds(10));
  }

  void TearDown() override { PlatformClientPosix::ShutDown(); }

  TaskRunner& GetTaskRunner() {
    return PlatformClientPosix::GetInstance()->GetTaskRunner();
  }
};

TEST_F(QuicheWebTransportTest, ConnectAndExchangeData) {
  TaskRunner& task_runner = GetTaskRunner();

  // 1. Create and start server.
  auto server = RunOnTaskRunner(
      task_runner, [&]() { return WebTransportServer::Create(task_runner); });
  ASSERT_NE(server, nullptr);

  std::promise<std::unique_ptr<WebTransportSession>> server_session_promise;
  auto server_session_future = server_session_promise.get_future();
  TestServerDelegate server_delegate(std::move(server_session_promise));

  RunOnTaskRunner(task_runner,
                  [&]() { server->SetDelegate(&server_delegate); });

  uint16_t port = RunOnTaskRunner(task_runner, [&]() {
    return server->Start(IPEndpoint{IPAddress::kV4LoopbackAddress(), 0});
  });
  ASSERT_NE(port, 0);

  std::string fingerprint = RunOnTaskRunner(
      task_runner, [&]() { return server->GetCertificateFingerprint(); });
  ASSERT_FALSE(fingerprint.empty());

  // 2. Create client and connect.
  auto client = RunOnTaskRunner(
      task_runner, [&]() { return WebTransportClient::Create(task_runner); });
  ASSERT_NE(client, nullptr);

  std::promise<std::unique_ptr<WebTransportSession>> client_session_promise;
  auto client_session_future = client_session_promise.get_future();

  WebTransportOptions options;
  options.server_certificate_hashes.push_back(fingerprint);

  const std::string url = std::format("https://127.0.0.1:{}/test", port);
  RunOnTaskRunner(task_runner, [&]() {
    client->Connect(
        url, options,
        [&client_session_promise](
            ErrorOr<std::unique_ptr<WebTransportSession>> session) {
          if (session.is_value()) {
            client_session_promise.set_value(std::move(session.value()));
          } else {
            client_session_promise.set_value(nullptr);
          }
        });
  });

  // 3. Wait for connection to be established on both sides.
  ASSERT_EQ(client_session_future.wait_for(kTestTimeout),
            std::future_status::ready);
  auto client_session = client_session_future.get();
  ASSERT_NE(client_session, nullptr);

  ASSERT_EQ(server_session_future.wait_for(kTestTimeout),
            std::future_status::ready);
  auto server_session = server_session_future.get();
  ASSERT_NE(server_session, nullptr);

  // Set delegate for client session to handle events.
  TestSessionDelegate client_session_delegate;
  RunOnTaskRunner(task_runner, [&]() {
    client_session->SetDelegate(&client_session_delegate);
  });

  // 4. Create bidirectional stream from client.
  std::promise<WebTransportStream*> server_stream_promise;
  auto server_stream_future = server_stream_promise.get_future();
  server_delegate.session_delegate().SetIncomingStreamPromise(
      std::move(server_stream_promise));

  std::unique_ptr<WebTransportStream> client_stream_owner;
  auto* client_stream =
      RunOnTaskRunner(task_runner, [&]() -> WebTransportStream* {
        auto stream_or = client_session->CreateOutgoingStream();
        if (!stream_or.is_value()) {
          ADD_FAILURE() << "CreateOutgoingStream failed: " << stream_or.error();
          return nullptr;
        }
        client_stream_owner = std::move(stream_or.value());
        return client_stream_owner.get();
      });
  ASSERT_NE(client_stream, nullptr);

  // Wait for server to receive the stream.
  ASSERT_EQ(server_stream_future.wait_for(kTestTimeout),
            std::future_status::ready);
  auto* server_stream = server_stream_future.get();
  ASSERT_NE(server_stream, nullptr);

  // 5. Send data from Client to Server.
  std::promise<std::string> server_read_promise;
  auto server_read_future = server_read_promise.get_future();
  TestStreamDelegate server_stream_delegate(std::move(server_read_promise));

  RunOnTaskRunner(task_runner, [&]() {
    server_stream->SetDelegate(&server_stream_delegate);
  });

  constexpr std::string_view kClientMessage = "ping from client";
  bool client_write_ok = RunOnTaskRunner(task_runner, [&]() {
    return client_stream->Write(
        ByteView(reinterpret_cast<const uint8_t*>(kClientMessage.data()),
                 kClientMessage.size()));
  });
  EXPECT_TRUE(client_write_ok);

  // Wait for server to receive it.
  ASSERT_EQ(server_read_future.wait_for(kTestTimeout),
            std::future_status::ready);
  EXPECT_EQ(server_read_future.get(), kClientMessage);

  // 6. Send data from Server to Client.
  std::promise<std::string> client_read_promise;
  auto client_read_future = client_read_promise.get_future();
  TestStreamDelegate client_stream_delegate(std::move(client_read_promise));

  RunOnTaskRunner(task_runner, [&]() {
    client_stream->SetDelegate(&client_stream_delegate);
  });

  constexpr std::string_view kServerMessage = "pong from server";
  bool server_write_ok = RunOnTaskRunner(task_runner, [&]() {
    return server_stream->Write(
        ByteView(reinterpret_cast<const uint8_t*>(kServerMessage.data()),
                 kServerMessage.size()));
  });
  EXPECT_TRUE(server_write_ok);

  // Wait for client to receive it.
  ASSERT_EQ(client_read_future.wait_for(kTestTimeout),
            std::future_status::ready);
  EXPECT_EQ(client_read_future.get(), kServerMessage);

  // 7. Clean teardown.
  std::promise<Error> client_close_promise;
  auto client_close_future = client_close_promise.get_future();
  client_session_delegate.SetSessionClosedPromise(
      std::move(client_close_promise));

  std::promise<Error> server_close_promise;
  auto server_close_future = server_close_promise.get_future();
  server_delegate.session_delegate().SetSessionClosedPromise(
      std::move(server_close_promise));

  RunOnTaskRunner(task_runner, [&]() {
    client_stream->Close();
    server_stream->Close();
  });

  RunOnTaskRunner(task_runner, [&]() {
    client_session->Close(Error::None());
    server_session->Close(Error::None());
  });

  ASSERT_EQ(client_close_future.wait_for(kTestTimeout),
            std::future_status::ready);
  EXPECT_TRUE(client_close_future.get().ok());
  ASSERT_EQ(server_close_future.wait_for(kTestTimeout),
            std::future_status::ready);
  EXPECT_TRUE(server_close_future.get().ok());

  RunOnTaskRunner(task_runner, [&]() {
    client_stream_owner.reset();
    client_session.reset();
    server_session.reset();
    client.reset();
    server->Stop();
    server.reset();
  });

  // Delegates must be notified before their stream is freed, so that a
  // non-owning holder can drop its pointer. `server_stream` in particular is
  // owned by the server session, so its delegate would otherwise be left with
  // a dangling pointer once the session goes away.
  EXPECT_EQ(client_stream_delegate.destroyed_count(), 1);
  EXPECT_EQ(client_stream_delegate.destroyed_stream(), client_stream);
  EXPECT_EQ(server_stream_delegate.destroyed_count(), 1);
  EXPECT_EQ(server_stream_delegate.destroyed_stream(), server_stream);
}

}  // namespace
}  // namespace openscreen
