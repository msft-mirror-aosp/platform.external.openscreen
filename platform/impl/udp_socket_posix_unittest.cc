// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/impl/udp_socket_posix.h"

#include <sys/socket.h>

#include <memory>
#include <utility>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "platform/api/time.h"
#include "platform/test/fake_clock.h"
#include "platform/test/fake_task_runner.h"
#include "platform/test/fake_udp_socket.h"

namespace openscreen {
namespace {

using testing::_;

TEST(UdpSocketPosixTest, SetsBufferSizes) {
  const uint8_t kIpV4AddrAny[4] = {};
  FakeClock clock(Clock::now());
  FakeTaskRunner task_runner(clock);
  testing::StrictMock<FakeUdpSocket::MockClient> client;
  ErrorOr<std::unique_ptr<UdpSocket>> create_result = UdpSocket::Create(
      task_runner, &client, IPEndpoint{IPAddress(kIpV4AddrAny), 0});
  ASSERT_TRUE(create_result) << create_result.error();
  const auto socket = std::move(create_result.value());
  EXPECT_CALL(client, OnBound(_)).Times(1);
  socket->Bind();

  constexpr size_t kTestReceiveBufferSize = 10000;
  constexpr size_t kTestSendBufferSize = 20000;
  socket->SetReceiveBufferSize(kTestReceiveBufferSize);
  socket->SetSendBufferSize(kTestSendBufferSize);

  int rcvbuf = 0;
  socklen_t optlen = sizeof(rcvbuf);
  int res =
      getsockopt(static_cast<UdpSocketPosix*>(socket.get())->GetHandle().fd,
                 SOL_SOCKET, SO_RCVBUF, &rcvbuf, &optlen);
  ASSERT_EQ(res, 0);
  // Linux kernel doubles the requested buffer size for bookkeeping, or sets
  // it to at least the requested value.
  EXPECT_GE(static_cast<size_t>(rcvbuf), kTestReceiveBufferSize);

  int sndbuf = 0;
  optlen = sizeof(sndbuf);
  res = getsockopt(static_cast<UdpSocketPosix*>(socket.get())->GetHandle().fd,
                   SOL_SOCKET, SO_SNDBUF, &sndbuf, &optlen);
  ASSERT_EQ(res, 0);
  EXPECT_GE(static_cast<size_t>(sndbuf), kTestSendBufferSize);
}

}  // namespace
}  // namespace openscreen
