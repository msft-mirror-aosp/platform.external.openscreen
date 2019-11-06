// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "discovery/mdns/mdns_receiver.h"

#include "discovery/mdns/mdns_records.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "platform/api/time.h"
#include "platform/test/fake_udp_socket.h"

namespace openscreen {
namespace discovery {

using openscreen::platform::FakeUdpSocket;
using openscreen::platform::TaskRunner;
using openscreen::platform::UdpPacket;
using testing::_;
using testing::Return;

class MockMdnsReceiverDelegate {
 public:
  MOCK_METHOD(void, OnMessageReceived, (const MdnsMessage&));
};

TEST(MdnsReceiverTest, ReceiveQuery) {
  // clang-format off
  const std::vector<uint8_t> kQueryBytes = {
      0x00, 0x01,  // ID = 1
      0x00, 0x00,  // FLAGS = None
      0x00, 0x01,  // Question count
      0x00, 0x00,  // Answer count
      0x00, 0x00,  // Authority count
      0x00, 0x00,  // Additional count
      // Question
      0x07, 't', 'e', 's', 't', 'i', 'n', 'g',
      0x05, 'l', 'o', 'c', 'a', 'l',
      0x00,
      0x00, 0x01,  // TYPE = A (1)
      0x00, 0x01,  // CLASS = IN (1)
  };
  // clang-format on

  std::unique_ptr<FakeUdpSocket> socket_info =
      FakeUdpSocket::CreateDefault(IPAddress::Version::kV4);
  MockMdnsReceiverDelegate delegate;
  MdnsReceiver receiver(socket_info.get());
  receiver.SetQueryCallback([&delegate](const MdnsMessage& message) {
    delegate.OnMessageReceived(message);
  });
  receiver.Start();

  MdnsQuestion question(DomainName{"testing", "local"}, DnsType::kA,
                        DnsClass::kIN, ResponseType::kMulticast);
  MdnsMessage message(1, MessageType::Query);
  message.AddQuestion(question);

  UdpPacket packet(kQueryBytes.size());
  packet.assign(kQueryBytes.data(), kQueryBytes.data() + kQueryBytes.size());
  packet.set_source(
      IPEndpoint{.address = IPAddress(192, 168, 1, 1), .port = 31337});
  packet.set_destination(
      IPEndpoint{.address = IPAddress(kDefaultMulticastGroupIPv4),
                 .port = kDefaultMulticastPort});

  // Imitate a call to OnRead from NetworkRunner by calling it manually here
  EXPECT_CALL(delegate, OnMessageReceived(message)).Times(1);
  receiver.OnRead(socket_info.get(), std::move(packet));

  receiver.Stop();
}

TEST(MdnsReceiverTest, ReceiveResponse) {
  // clang-format off
  const std::vector<uint8_t> kResponseBytes = {
      0x00, 0x01,  // ID = 1
      0x84, 0x00,  // FLAGS = AA | RESPONSE
      0x00, 0x00,  // Question count
      0x00, 0x01,  // Answer count
      0x00, 0x00,  // Authority count
      0x00, 0x00,  // Additional count
      // Answer
      0x07, 't', 'e', 's', 't', 'i', 'n', 'g',
      0x05, 'l', 'o', 'c', 'a', 'l',
      0x00,
      0x00, 0x01,              // TYPE = A (1)
      0x00, 0x01,              // CLASS = IN (1)
      0x00, 0x00, 0x00, 0x78,  // TTL = 120 seconds
      0x00, 0x04,              // RDLENGTH = 4 bytes
      0xac, 0x00, 0x00, 0x01,  // 172.0.0.1
  };

  constexpr uint8_t kIPv6AddressBytes[] = {
      0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x02, 0x02, 0xb3, 0xff, 0xfe, 0x1e, 0x83, 0x29,
  };
  // clang-format on

  std::unique_ptr<FakeUdpSocket> socket_info =
      FakeUdpSocket::CreateDefault(IPAddress::Version::kV6);
  MockMdnsReceiverDelegate delegate;
  MdnsReceiver receiver(socket_info.get());
  receiver.SetResponseCallback([&delegate](const MdnsMessage& message) {
    delegate.OnMessageReceived(message);
  });
  receiver.Start();

  MdnsRecord record(DomainName{"testing", "local"}, DnsType::kA, DnsClass::kIN,
                    RecordType::kShared, std::chrono::seconds(120),
                    ARecordRdata(IPAddress{172, 0, 0, 1}));
  MdnsMessage message(1, MessageType::Response);
  message.AddAnswer(record);

  UdpPacket packet(kResponseBytes.size());
  packet.assign(kResponseBytes.data(),
                kResponseBytes.data() + kResponseBytes.size());
  packet.set_source(
      IPEndpoint{.address = IPAddress(kIPv6AddressBytes), .port = 31337});
  packet.set_destination(
      IPEndpoint{.address = IPAddress(kDefaultMulticastGroupIPv6),
                 .port = kDefaultMulticastPort});

  // Imitate a call to OnRead from NetworkRunner by calling it manually here
  EXPECT_CALL(delegate, OnMessageReceived(message)).Times(1);
  receiver.OnRead(socket_info.get(), std::move(packet));

  receiver.Stop();
}

}  // namespace discovery
}  // namespace openscreen