// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "discovery/mdns/public/simple_mdns_writer.h"

#include <chrono>
#include <vector>

#include "discovery/common/config.h"
#include "discovery/mdns/public/simple_mdns_reader.h"
#include "gtest/gtest.h"
#include "platform/base/span.h"

namespace openscreen::discovery {
namespace {

constexpr std::chrono::seconds kTtl(120);

TEST(SimpleMdnsWriterTest, WriteQuery) {
  MdnsMessage message(1, MessageType::Query);
  MdnsQuestion question(DomainName{"testing", "local"}, DnsType::kA,
                        DnsClass::kIN, ResponseType::kMulticast);
  message.AddQuestion(question);

  ErrorOr<std::vector<uint8_t>> serialized = SimpleMdnsWriter::Write(message);
  ASSERT_TRUE(serialized.is_value());

  Config config;
  SimpleMdnsReader reader(config, ByteView(serialized.value()));
  ErrorOr<MdnsMessage> parsed = reader.Read();
  ASSERT_TRUE(parsed.is_value());

  EXPECT_EQ(parsed.value().id(), 1);
  EXPECT_EQ(parsed.value().type(), MessageType::Query);
  ASSERT_EQ(parsed.value().questions().size(), 1u);
  EXPECT_EQ(parsed.value().questions()[0].name(),
            DomainName({"testing", "local"}));
  EXPECT_EQ(parsed.value().questions()[0].dns_type(), DnsType::kA);
  EXPECT_EQ(parsed.value().questions()[0].dns_class(), DnsClass::kIN);
  EXPECT_EQ(parsed.value().questions()[0].response_type(),
            ResponseType::kMulticast);
}

TEST(SimpleMdnsWriterTest, WriteResponseWithARecord) {
  MdnsMessage message(2, MessageType::Response);
  MdnsRecord record(DomainName{"testing", "local"}, DnsType::kA, DnsClass::kIN,
                    RecordType::kUnique, kTtl,
                    ARecordRdata(IPAddress(192, 168, 1, 100)));
  message.AddAnswer(record);

  ErrorOr<std::vector<uint8_t>> serialized = SimpleMdnsWriter::Write(message);
  ASSERT_TRUE(serialized.is_value());

  Config config;
  SimpleMdnsReader reader(config, ByteView(serialized.value()));
  ErrorOr<MdnsMessage> parsed = reader.Read();
  ASSERT_TRUE(parsed.is_value());

  EXPECT_EQ(parsed.value().id(), 2);
  EXPECT_EQ(parsed.value().type(), MessageType::Response);
  ASSERT_EQ(parsed.value().answers().size(), 1u);
  EXPECT_EQ(parsed.value().answers()[0].name(),
            DomainName({"testing", "local"}));
  EXPECT_EQ(parsed.value().answers()[0].dns_type(), DnsType::kA);
  EXPECT_EQ(parsed.value().answers()[0].record_type(), RecordType::kUnique);
  EXPECT_EQ(parsed.value().answers()[0].ttl(), kTtl);

  const auto& a = std::get<ARecordRdata>(parsed.value().answers()[0].rdata());
  EXPECT_EQ(a.ipv4_address(), IPAddress(192, 168, 1, 100));
}

TEST(SimpleMdnsWriterTest, WriteResponseWithAAAARecord) {
  const uint8_t ipv6_bytes[] = {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x02, 0x02, 0xb3, 0xff, 0xfe, 0x1e, 0x83, 0x29};
  MdnsMessage message(3, MessageType::Response);
  MdnsRecord record(
      DomainName{"testing", "local"}, DnsType::kAAAA, DnsClass::kIN,
      RecordType::kShared, kTtl,
      AAAARecordRdata(IPAddress(IPAddress::Version::kV6, ipv6_bytes)));
  message.AddAnswer(record);

  ErrorOr<std::vector<uint8_t>> serialized = SimpleMdnsWriter::Write(message);
  ASSERT_TRUE(serialized.is_value());

  Config config;
  SimpleMdnsReader reader(config, ByteView(serialized.value()));
  ErrorOr<MdnsMessage> parsed = reader.Read();
  ASSERT_TRUE(parsed.is_value());

  ASSERT_EQ(parsed.value().answers().size(), 1u);
  EXPECT_EQ(parsed.value().answers()[0].dns_type(), DnsType::kAAAA);
  EXPECT_EQ(parsed.value().answers()[0].record_type(), RecordType::kShared);

  const auto& aaaa =
      std::get<AAAARecordRdata>(parsed.value().answers()[0].rdata());
  EXPECT_EQ(aaaa.ipv6_address(),
            IPAddress(IPAddress::Version::kV6, ipv6_bytes));
}

TEST(SimpleMdnsWriterTest, WriteResponseWithPtrRecord) {
  MdnsMessage message(4, MessageType::Response);
  MdnsRecord record(
      DomainName{"_service", "_tcp", "local"}, DnsType::kPTR, DnsClass::kIN,
      RecordType::kShared, kTtl,
      PtrRecordRdata(DomainName{"instance", "_service", "_tcp", "local"}));
  message.AddAnswer(record);

  ErrorOr<std::vector<uint8_t>> serialized = SimpleMdnsWriter::Write(message);
  ASSERT_TRUE(serialized.is_value());

  Config config;
  SimpleMdnsReader reader(config, ByteView(serialized.value()));
  ErrorOr<MdnsMessage> parsed = reader.Read();
  ASSERT_TRUE(parsed.is_value());

  ASSERT_EQ(parsed.value().answers().size(), 1u);
  EXPECT_EQ(parsed.value().answers()[0].dns_type(), DnsType::kPTR);

  const auto& ptr =
      std::get<PtrRecordRdata>(parsed.value().answers()[0].rdata());
  EXPECT_EQ(ptr.ptr_domain(),
            DomainName({"instance", "_service", "_tcp", "local"}));
}

TEST(SimpleMdnsWriterTest, WriteResponseWithTxtRecord) {
  MdnsMessage message(5, MessageType::Response);
  MdnsRecord record(
      DomainName{"testing", "local"}, DnsType::kTXT, DnsClass::kIN,
      RecordType::kUnique, kTtl,
      TxtRecordRdata(std::vector<TxtRecordRdata::Entry>{
          TxtRecordRdata::Entry({'k', 'e', 'y', '=', '1'}),
          TxtRecordRdata::Entry({'f', 'o', 'o', '=', 'b', 'a', 'r'}),
      }));
  message.AddAnswer(record);

  ErrorOr<std::vector<uint8_t>> serialized = SimpleMdnsWriter::Write(message);
  ASSERT_TRUE(serialized.is_value());

  Config config;
  SimpleMdnsReader reader(config, ByteView(serialized.value()));
  ErrorOr<MdnsMessage> parsed = reader.Read();
  ASSERT_TRUE(parsed.is_value());

  ASSERT_EQ(parsed.value().answers().size(), 1u);
  EXPECT_EQ(parsed.value().answers()[0].dns_type(), DnsType::kTXT);

  const auto& txt =
      std::get<TxtRecordRdata>(parsed.value().answers()[0].rdata());
  const std::vector<std::vector<uint8_t>> expected_texts = {
      {'k', 'e', 'y', '=', '1'},
      {'f', 'o', 'o', '=', 'b', 'a', 'r'},
  };
  EXPECT_EQ(txt.texts(), expected_texts);
}

TEST(SimpleMdnsWriterTest, WriteResponseWithSrvRecord) {
  MdnsMessage message(6, MessageType::Response);
  MdnsRecord record(DomainName{"testing", "local"}, DnsType::kSRV,
                    DnsClass::kIN, RecordType::kUnique, kTtl,
                    SrvRecordRdata(10, 20, 8080, DomainName{"host", "local"}));
  message.AddAnswer(record);

  ErrorOr<std::vector<uint8_t>> serialized = SimpleMdnsWriter::Write(message);
  ASSERT_TRUE(serialized.is_value());

  Config config;
  SimpleMdnsReader reader(config, ByteView(serialized.value()));
  ErrorOr<MdnsMessage> parsed = reader.Read();
  ASSERT_TRUE(parsed.is_value());

  ASSERT_EQ(parsed.value().answers().size(), 1u);
  EXPECT_EQ(parsed.value().answers()[0].dns_type(), DnsType::kSRV);

  const auto& srv =
      std::get<SrvRecordRdata>(parsed.value().answers()[0].rdata());
  EXPECT_EQ(srv.priority(), 10);
  EXPECT_EQ(srv.weight(), 20);
  EXPECT_EQ(srv.port(), 8080);
  EXPECT_EQ(srv.target(), DomainName({"host", "local"}));
}

}  // namespace
}  // namespace openscreen::discovery
