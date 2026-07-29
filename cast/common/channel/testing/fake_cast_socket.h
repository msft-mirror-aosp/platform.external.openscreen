// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_COMMON_CHANNEL_TESTING_FAKE_CAST_SOCKET_H_
#define CAST_COMMON_CHANNEL_TESTING_FAKE_CAST_SOCKET_H_

#include <memory>
#include <utility>
#include <vector>

#include "cast/common/channel/proto/cast_channel.pb.h"
#include "cast/common/public/cast_socket.h"
#include "gmock/gmock.h"
#include "platform/test/mock_tls_connection.h"
#include "util/raw_ptr.h"

namespace openscreen::cast {

class MockCastSocketClient final : public CastSocket::Client {
 public:
  ~MockCastSocketClient() override = default;

  MOCK_METHOD(void,
              OnError,
              (CastSocket * socket, const Error& error),
              (override));
  MOCK_METHOD(void,
              OnMessage,
              (CastSocket * socket, proto::CastMessage message),
              (override));
};

struct FakeCastSocket {
  FakeCastSocket()
      : FakeCastSocket({{10, 0, 1, 7}, 1234}, {{10, 0, 1, 9}, 4321}) {}
  FakeCastSocket(const IPEndpoint& local_endpoint,
                 const IPEndpoint& remote_endpoint)
      : FakeCastSocket(local_endpoint,
                       remote_endpoint,
                       std::make_unique<MockTlsConnection>(local_endpoint,
                                                           remote_endpoint)) {}

  FakeCastSocket(const IPEndpoint& local_endpoint,
                 const IPEndpoint& remote_endpoint,
                 std::unique_ptr<MockTlsConnection> conn)
      : local_endpoint(local_endpoint),
        remote_endpoint(remote_endpoint),
        mock_client(),
        socket(
            [&] {
              connection = conn.get();
              return std::move(conn);
            }(),
            &mock_client) {}

 public:
  IPEndpoint local_endpoint;
  IPEndpoint remote_endpoint;
  MockCastSocketClient mock_client;
  raw_ptr<MockTlsConnection> connection;
  CastSocket socket;
};

// Two FakeCastSockets that are piped together via their MockTlsConnection
// read/write methods.  Calling SendMessage on `socket` will result in an
// OnMessage callback on `mock_peer_client` and vice versa for `peer_socket` and
// `mock_client`.
struct FakeCastSocketPair {
  FakeCastSocketPair()
      : FakeCastSocketPair({{10, 0, 1, 7}, 1234}, {{10, 0, 1, 9}, 4321}) {}

  FakeCastSocketPair(const IPEndpoint& local_endpoint,
                     const IPEndpoint& remote_endpoint)
      : local_endpoint(local_endpoint), remote_endpoint(remote_endpoint) {
    using ::testing::_;

    auto moved_connection =
        std::make_unique<::testing::NiceMock<MockTlsConnection>>(
            local_endpoint, remote_endpoint);
    auto* connection = moved_connection.get();
    socket =
        std::make_unique<CastSocket>(std::move(moved_connection), &mock_client);

    auto moved_peer = std::make_unique<::testing::NiceMock<MockTlsConnection>>(
        remote_endpoint, local_endpoint);
    auto* peer_connection = moved_peer.get();
    peer_socket =
        std::make_unique<CastSocket>(std::move(moved_peer), &mock_peer_client);

    ON_CALL(*connection, Send(_))
        .WillByDefault([peer_connection](ByteView data) {
          peer_connection->OnRead(
              std::vector<uint8_t>(data.begin(), data.end()));
          return true;
        });
    ON_CALL(*peer_connection, Send(_))
        .WillByDefault([connection](ByteView data) {
          connection->OnRead(std::vector<uint8_t>(data.begin(), data.end()));
          return true;
        });
  }
  ~FakeCastSocketPair() = default;

  IPEndpoint local_endpoint;
  IPEndpoint remote_endpoint;

  MockCastSocketClient mock_client;
  std::unique_ptr<CastSocket> socket;

  MockCastSocketClient mock_peer_client;
  std::unique_ptr<CastSocket> peer_socket;
};

}  // namespace openscreen::cast

#endif  // CAST_COMMON_CHANNEL_TESTING_FAKE_CAST_SOCKET_H_
