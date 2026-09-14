// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "discovery/mdns/public/simple_mdns_reader.h"

#include <chrono>
#include <vector>

#include "discovery/common/config.h"
#include "discovery/mdns/public/mdns_records.h"
#include "gtest/gtest.h"

namespace openscreen::discovery {
namespace {

TEST(SimpleMdnsReaderTest, ParseQueryPacket) {
  // clang-format off
  const std::vector<uint8_t> kQueryBytes = {
      0x00, 0x01,  // ID = 1
      0x00, 0x00,  // FLAGS = None
      0x00, 0x01,  // Question count = 1
      0x00, 0x00,  // Answer count = 0
      0x00, 0x00,  // Authority count = 0
      0x00, 0x00,  // Additional count = 0
      // Question: testing.local, type A (1), class IN (1)
      0x07, 't', 'e', 's', 't', 'i', 'n', 'g',
      0x05, 'l', 'o', 'c', 'a', 'l',
      0x00,
      0x00, 0x01,  // TYPE = A (1)
      0x00, 0x01,  // CLASS = IN (1)
  };
  // clang-format on

  Config config;
  SimpleMdnsReader reader(config, ByteView(kQueryBytes));
  ErrorOr<MdnsMessage> result = reader.Read();
  ASSERT_TRUE(result.is_value());

  const MdnsMessage& msg = result.value();
  EXPECT_EQ(msg.id(), 1);
  EXPECT_EQ(msg.type(), MessageType::Query);
  ASSERT_EQ(msg.questions().size(), 1u);
  EXPECT_EQ(msg.questions()[0].name(), DomainName({"testing", "local"}));
  EXPECT_EQ(msg.questions()[0].dns_type(), DnsType::kA);
  EXPECT_EQ(msg.questions()[0].dns_class(), DnsClass::kIN);
  EXPECT_EQ(msg.questions()[0].response_type(), ResponseType::kMulticast);
}

TEST(SimpleMdnsReaderTest, ParseResponsePacketWithARecord) {
  // clang-format off
  const std::vector<uint8_t> kResponseBytes = {
      0x00, 0x01,  // ID = 1
      0x84, 0x00,  // FLAGS = AA | RESPONSE
      0x00, 0x00,  // Question count = 0
      0x00, 0x01,  // Answer count = 1
      0x00, 0x00,  // Authority count = 0
      0x00, 0x00,  // Additional count = 0
      // Answer: testing.local, type A, class IN, TTL 120, 172.0.0.1
      0x07, 't', 'e', 's', 't', 'i', 'n', 'g',
      0x05, 'l', 'o', 'c', 'a', 'l',
      0x00,
      0x00, 0x01,              // TYPE = A (1)
      0x00, 0x01,              // CLASS = IN (1)
      0x00, 0x00, 0x00, 0x78,  // TTL = 120 seconds
      0x00, 0x04,              // RDLENGTH = 4 bytes
      0xac, 0x00, 0x00, 0x01,  // 172.0.0.1
  };
  // clang-format on

  Config config;
  SimpleMdnsReader reader(config, ByteView(kResponseBytes));
  ErrorOr<MdnsMessage> result = reader.Read();
  ASSERT_TRUE(result.is_value());

  const MdnsMessage& msg = result.value();
  EXPECT_EQ(msg.id(), 1);
  EXPECT_EQ(msg.type(), MessageType::Response);
  ASSERT_EQ(msg.answers().size(), 1u);
  EXPECT_EQ(msg.answers()[0].name(), DomainName({"testing", "local"}));
  EXPECT_EQ(msg.answers()[0].dns_type(), DnsType::kA);
  EXPECT_EQ(msg.answers()[0].dns_class(), DnsClass::kIN);
  EXPECT_EQ(msg.answers()[0].ttl(), std::chrono::seconds(120));

  const auto& a_record = std::get<ARecordRdata>(msg.answers()[0].rdata());
  EXPECT_EQ(a_record.ipv4_address(), IPAddress(172, 0, 0, 1));
}

TEST(SimpleMdnsReaderTest, ParseAAAARecord) {
  // clang-format off
  const std::vector<uint8_t> kResponseBytes = {
      0x00, 0x02,  // ID = 2
      0x84, 0x00,  // FLAGS = AA | RESPONSE
      0x00, 0x00,  // Question count = 0
      0x00, 0x01,  // Answer count = 1
      0x00, 0x00,  // Authority count = 0
      0x00, 0x00,  // Additional count = 0
      // Answer: testing.local, type AAAA, class IN, TTL 120
      0x07, 't', 'e', 's', 't', 'i', 'n', 'g',
      0x05, 'l', 'o', 'c', 'a', 'l',
      0x00,
      0x00, 0x1c,              // TYPE = AAAA (28)
      0x80, 0x01,              // CLASS = IN (1) | CACHE_FLUSH
      0x00, 0x00, 0x00, 0x78,  // TTL = 120 seconds
      0x00, 0x10,              // RDLENGTH = 16 bytes
      0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x02, 0x02, 0xb3, 0xff, 0xfe, 0x1e, 0x83, 0x29,
  };
  // clang-format on

  Config config;
  SimpleMdnsReader reader(config, ByteView(kResponseBytes));
  ErrorOr<MdnsMessage> result = reader.Read();
  ASSERT_TRUE(result.is_value());

  const MdnsMessage& msg = result.value();
  ASSERT_EQ(msg.answers().size(), 1u);
  EXPECT_EQ(msg.answers()[0].dns_type(), DnsType::kAAAA);
  EXPECT_EQ(msg.answers()[0].record_type(), RecordType::kUnique);

  const auto& aaaa = std::get<AAAARecordRdata>(msg.answers()[0].rdata());
  const uint8_t expected_ip[] = {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00,
                                 0x00, 0x00, 0x02, 0x02, 0xb3, 0xff,
                                 0xfe, 0x1e, 0x83, 0x29};
  EXPECT_EQ(aaaa.ipv6_address(),
            IPAddress(IPAddress::Version::kV6, expected_ip));
}

TEST(SimpleMdnsReaderTest, ParsePtrRecord) {
  // clang-format off
  const std::vector<uint8_t> kResponseBytes = {
      0x00, 0x03,  // ID = 3
      0x84, 0x00,  // FLAGS = AA | RESPONSE
      0x00, 0x00,  // Question count = 0
      0x00, 0x01,  // Answer count = 1
      0x00, 0x00,  // Authority count = 0
      0x00, 0x00,  // Additional count = 0
      // Answer: _service._tcp.local, type PTR, class IN, TTL 120
      0x08, '_', 's', 'e', 'r', 'v', 'i', 'c', 'e',
      0x04, '_', 't', 'c', 'p',
      0x05, 'l', 'o', 'c', 'a', 'l',
      0x00,
      0x00, 0x0c,              // TYPE = PTR (12)
      0x00, 0x01,              // CLASS = IN (1)
      0x00, 0x00, 0x00, 0x78,  // TTL = 120 seconds
      0x00, 0x08,              // RDLENGTH = 8 bytes
      0x05, 'i', 'n', 's', 't', '1',
      0xc0, 0x0c,              // Pointer to _service._tcp.local
  };
  // clang-format on

  Config config;
  SimpleMdnsReader reader(config, ByteView(kResponseBytes));
  ErrorOr<MdnsMessage> result = reader.Read();
  ASSERT_TRUE(result.is_value());

  const MdnsMessage& msg = result.value();
  ASSERT_EQ(msg.answers().size(), 1u);
  EXPECT_EQ(msg.answers()[0].dns_type(), DnsType::kPTR);
  const auto& ptr = std::get<PtrRecordRdata>(msg.answers()[0].rdata());
  EXPECT_EQ(ptr.ptr_domain(),
            DomainName({"inst1", "_service", "_tcp", "local"}));
}

TEST(SimpleMdnsReaderTest, ParseTxtRecord) {
  // clang-format off
  const std::vector<uint8_t> kResponseBytes = {
      0x00, 0x04,  // ID = 4
      0x84, 0x00,  // FLAGS = AA | RESPONSE
      0x00, 0x00,  // Question count = 0
      0x00, 0x01,  // Answer count = 1
      0x00, 0x00,  // Authority count = 0
      0x00, 0x00,  // Additional count = 0
      // Answer: testing.local, type TXT, class IN, TTL 120
      0x07, 't', 'e', 's', 't', 'i', 'n', 'g',
      0x05, 'l', 'o', 'c', 'a', 'l',
      0x00,
      0x00, 0x10,              // TYPE = TXT (16)
      0x80, 0x01,              // CLASS = IN (1) | CACHE_FLUSH
      0x00, 0x00, 0x00, 0x78,  // TTL = 120 seconds
      0x00, 0x0c,              // RDLENGTH = 12 bytes
      0x05, 'f', 'o', 'o', '=', '1',
      0x05, 'b', 'a', 'r', '=', '2',
  };
  // clang-format on

  Config config;
  SimpleMdnsReader reader(config, ByteView(kResponseBytes));
  ErrorOr<MdnsMessage> result = reader.Read();
  ASSERT_TRUE(result.is_value());

  const MdnsMessage& msg = result.value();
  ASSERT_EQ(msg.answers().size(), 1u);
  EXPECT_EQ(msg.answers()[0].dns_type(), DnsType::kTXT);
  const auto& txt = std::get<TxtRecordRdata>(msg.answers()[0].rdata());
  const std::vector<std::vector<uint8_t>> expected_texts = {
      {'f', 'o', 'o', '=', '1'},
      {'b', 'a', 'r', '=', '2'},
  };
  EXPECT_EQ(txt.texts(), expected_texts);
}

TEST(SimpleMdnsReaderTest, ParseSrvRecord) {
  // clang-format off
  const std::vector<uint8_t> kResponseBytes = {
      0x00, 0x05,  // ID = 5
      0x84, 0x00,  // FLAGS = AA | RESPONSE
      0x00, 0x00,  // Question count = 0
      0x00, 0x01,  // Answer count = 1
      0x00, 0x00,  // Authority count = 0
      0x00, 0x00,  // Additional count = 0
      // Answer: testing.local, type SRV, class IN, TTL 120
      0x07, 't', 'e', 's', 't', 'i', 'n', 'g',
      0x05, 'l', 'o', 'c', 'a', 'l',
      0x00,
      0x00, 0x21,              // TYPE = SRV (33)
      0x80, 0x01,              // CLASS = IN (1) | CACHE_FLUSH
      0x00, 0x00, 0x00, 0x78,  // TTL = 120 seconds
      0x00, 0x0f,              // RDLENGTH = 15 bytes
      0x00, 0x00,              // Priority = 0
      0x00, 0x00,              // Weight = 0
      0x1f, 0x40,              // Port = 8000
      0x06, 't', 'a', 'r', 'g', 'e', 't',
      0xc0, 0x14,              // Pointer to local
  };
  // clang-format on

  Config config;
  SimpleMdnsReader reader(config, ByteView(kResponseBytes));
  ErrorOr<MdnsMessage> result = reader.Read();
  ASSERT_TRUE(result.is_value());

  const MdnsMessage& msg = result.value();
  ASSERT_EQ(msg.answers().size(), 1u);
  EXPECT_EQ(msg.answers()[0].dns_type(), DnsType::kSRV);
  const auto& srv = std::get<SrvRecordRdata>(msg.answers()[0].rdata());
  EXPECT_EQ(srv.port(), 8000);
  EXPECT_EQ(srv.target(), DomainName({"target", "local"}));
}

TEST(SimpleMdnsReaderTest, TruncatedPacketFailsGracefully) {
  const std::vector<uint8_t> kInvalidBytes = {0x00, 0x01, 0x00};
  Config config;
  SimpleMdnsReader reader(config, ByteView(kInvalidBytes));
  ErrorOr<MdnsMessage> result = reader.Read();
  EXPECT_TRUE(result.is_error());
  EXPECT_EQ(result.error().code(), Error::Code::kMdnsReadFailure);
}

TEST(SimpleMdnsReaderTest, ExceedsMaxRdataSizeReturnsError) {
  // clang-format off
  const std::vector<uint8_t> kResponseBytes = {
      0x00, 0x06,  // ID = 6
      0x84, 0x00,  // FLAGS = AA | RESPONSE
      0x00, 0x00,  // Question count = 0
      0x00, 0x01,  // Answer count = 1
      0x00, 0x00,  // Authority count = 0
      0x00, 0x00,  // Additional count = 0
      // Answer: testing.local, type TXT, class IN, TTL 120
      0x07, 't', 'e', 's', 't', 'i', 'n', 'g',
      0x05, 'l', 'o', 'c', 'a', 'l',
      0x00,
      0x00, 0x10,              // TYPE = TXT (16)
      0x80, 0x01,              // CLASS = IN (1) | CACHE_FLUSH
      0x00, 0x00, 0x00, 0x78,  // TTL = 120 seconds
      0x00, 0x0c,              // RDLENGTH = 12 bytes
      0x05, 'f', 'o', 'o', '=', '1',
      0x05, 'b', 'a', 'r', '=', '2',
  };
  // clang-format on

  Config config;
  config.maximum_valid_rdata_size = 5;  // 12 bytes > 5
  SimpleMdnsReader reader(config, ByteView(kResponseBytes));
  ErrorOr<MdnsMessage> result = reader.Read();
  EXPECT_TRUE(result.is_error());
  EXPECT_EQ(result.error().code(), Error::Code::kMdnsReadFailure);
}

TEST(SimpleMdnsReaderTest, StaticReadWithByteView) {
  // clang-format off
  const std::vector<uint8_t> kQueryBytes = {
      0x00, 0x01,  // ID = 1
      0x00, 0x00,  // FLAGS = None
      0x00, 0x01,  // Question count = 1
      0x00, 0x00,  // Answer count = 0
      0x00, 0x00,  // Authority count = 0
      0x00, 0x00,  // Additional count = 0
      // Question: testing.local, type A (1), class IN (1)
      0x07, 't', 'e', 's', 't', 'i', 'n', 'g',
      0x05, 'l', 'o', 'c', 'a', 'l',
      0x00,
      0x00, 0x01,  // TYPE = A (1)
      0x00, 0x01,  // CLASS = IN (1)
  };
  // clang-format on

  Config config;
  ErrorOr<MdnsMessage> result =
      SimpleMdnsReader::Read(config, ByteView(kQueryBytes));
  ASSERT_TRUE(result.is_value());

  const MdnsMessage& msg = result.value();
  EXPECT_EQ(msg.id(), 1);
  EXPECT_EQ(msg.type(), MessageType::Query);
  ASSERT_EQ(msg.questions().size(), 1u);
  EXPECT_EQ(msg.questions()[0].name(), DomainName({"testing", "local"}));
}

}  // namespace
}  // namespace openscreen::discovery
