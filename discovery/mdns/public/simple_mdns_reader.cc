// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "discovery/mdns/public/simple_mdns_reader.h"

#include <bitset>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "discovery/common/config.h"
#include "discovery/mdns/public/mdns_constants.h"
#include "discovery/mdns/public/mdns_records.h"
#include "discovery/mdns/public/mdns_wire.rs.h"
#include "platform/base/span.h"
#include "util/osp_logging.h"

namespace openscreen::discovery {
namespace {

DomainName ConvertLabels(const rust::Vec<rust::String>& labels) {
  std::vector<std::string> std_labels;
  std_labels.reserve(labels.size());
  for (const auto& l : labels) {
    std_labels.emplace_back(std::string(l));
  }
  return DomainName(std::move(std_labels));
}

ErrorOr<MdnsQuestion> ConvertQuestion(const DnsQuestion& q) {
  DomainName name = ConvertLabels(q.name_labels);
  if (name.empty()) {
    return Error::Code::kMdnsReadFailure;
  }
  // DnsType and DnsClass are scoped enums with fixed underlying type uint16_t:
  //   enum class DnsType : uint16_t
  //   enum class DnsClass : uint16_t
  // Per C++17 [expr.static.cast] / [dcl.enum], static_cast from an integer to
  // an enumeration with a fixed underlying type is well-defined as long as the
  // value fits within the range of the underlying type. Since qtype and qclass
  // are uint16_t, these static_casts cannot produce undefined behavior.
  return MdnsQuestion::TryCreate(std::move(name), static_cast<DnsType>(q.qtype),
                                 static_cast<DnsClass>(q.qclass),
                                 q.is_unicast_response
                                     ? ResponseType::kUnicast
                                     : ResponseType::kMulticast);
}

ErrorOr<MdnsRecord> ConvertRecord(const DnsRecord& r,
                                  size_t max_allowed_rdata_size) {
  if (r.rdata_bytes.size() > max_allowed_rdata_size) {
    return Error::Code::kMdnsReadFailure;
  }

  DomainName name = ConvertLabels(r.name_labels);
  // DnsType and DnsClass are scoped enums with fixed underlying type uint16_t.
  // Converting uint16_t to DnsType/DnsClass is well-defined under
  // [expr.static.cast]. Unrecognized DnsType values fall through to the default
  // branch, producing RawRecordRdata.
  const auto dns_type = static_cast<DnsType>(r.rtype);
  const auto dns_class = static_cast<DnsClass>(r.rclass);
  const auto record_type =
      r.is_cache_flush ? RecordType::kUnique : RecordType::kShared;
  const std::chrono::seconds ttl(r.ttl_seconds);

  switch (dns_type) {
    case DnsType::kA: {
      if (r.rdata_bytes.size() != IPAddress::kV4Size) {
        return Error::Code::kMdnsReadFailure;
      }
      IPAddress ip(IPAddress::Version::kV4, r.rdata_bytes.data());
      auto record = MdnsRecord::TryCreate(std::move(name), dns_type, dns_class,
                                          record_type, ttl, ARecordRdata(ip));
      if (record.is_error()) {
        return Error::Code::kMdnsReadFailure;
      }
      return std::move(record.value());
    }
    case DnsType::kAAAA: {
      if (r.rdata_bytes.size() != IPAddress::kV6Size) {
        return Error::Code::kMdnsReadFailure;
      }
      IPAddress ip(IPAddress::Version::kV6, r.rdata_bytes.data());
      auto record =
          MdnsRecord::TryCreate(std::move(name), dns_type, dns_class,
                                record_type, ttl, AAAARecordRdata(ip));
      if (record.is_error()) {
        return Error::Code::kMdnsReadFailure;
      }
      return std::move(record.value());
    }
    case DnsType::kPTR: {
      DomainName ptr_target = ConvertLabels(r.ptr_target_labels);
      auto record = MdnsRecord::TryCreate(
          std::move(name), dns_type, dns_class, record_type, ttl,
          PtrRecordRdata(std::move(ptr_target)));
      if (record.is_error()) {
        return Error::Code::kMdnsReadFailure;
      }
      return std::move(record.value());
    }
    case DnsType::kSRV: {
      DomainName srv_target = ConvertLabels(r.srv_target_labels);
      auto record = MdnsRecord::TryCreate(
          std::move(name), dns_type, dns_class, record_type, ttl,
          SrvRecordRdata(r.srv_priority, r.srv_weight, r.srv_port,
                         std::move(srv_target)));
      if (record.is_error()) {
        return Error::Code::kMdnsReadFailure;
      }
      return std::move(record.value());
    }
    case DnsType::kTXT: {
      std::vector<TxtRecordRdata::Entry> entries;
      ByteView remaining(r.rdata_bytes.data(), r.rdata_bytes.size());
      while (!remaining.empty()) {
        const uint8_t len = remaining.front();
        remaining = remaining.subspan(1);
        if (remaining.size() < len) {
          return Error::Code::kMdnsReadFailure;
        }
        entries.emplace_back(remaining.begin(), remaining.begin() + len);
        remaining = remaining.subspan(len);
      }
      ErrorOr<TxtRecordRdata> txt =
          TxtRecordRdata::TryCreate(std::move(entries));
      if (txt.is_error()) {
        return txt.error();
      }
      auto record =
          MdnsRecord::TryCreate(std::move(name), dns_type, dns_class,
                                record_type, ttl, std::move(txt.value()));
      if (record.is_error()) {
        return Error::Code::kMdnsReadFailure;
      }
      return std::move(record.value());
    }
    case DnsType::kNSEC: {
      std::vector<DnsType> types;
      ByteView remaining(r.rdata_bytes.data(), r.rdata_bytes.size());
      // RFC 4034 Section 4.1.2: Type Bit Maps field.
      // Each block in the Type Bit Maps consists of:
      //   - Window block number (1 octet): 0 to 255.
      //   - Bitmap length (1 octet): 1 to 32 octets.
      //   - Bitmap data (bitmap length octets).
      //
      // In each bitmap octet, bit 0 is the most significant bit (0x80) and
      // bit 7 is the least significant bit (0x01). Bit position N corresponds
      // to RR type: (window << 8) | (byte_index * 8 + bit_offset).
      while (remaining.size() >= 2) {
        const uint8_t window = remaining[0];
        const uint8_t bitmap_len = remaining[1];
        remaining = remaining.subspan(2);
        if (remaining.size() < bitmap_len || bitmap_len == 0 ||
            bitmap_len > 32) {
          return Error::Code::kMdnsReadFailure;
        }
        ByteView bitmap = remaining.first(bitmap_len);
        remaining = remaining.subspan(bitmap_len);

        for (size_t byte_idx = 0; byte_idx < bitmap.size(); ++byte_idx) {
          const std::bitset<8> bits(bitmap[byte_idx]);
          const uint16_t base_type = (static_cast<uint16_t>(window) << 8) |
                                     static_cast<uint16_t>(byte_idx * 8);
          for (size_t bit = 0; bit < 8; ++bit) {
            // std::bitset bit 7 corresponds to MSB (0x80), bit 0 to LSB (0x01).
            if (bits.test(7 - bit)) {
              types.push_back(static_cast<DnsType>(base_type + bit));
            }
          }
        }
      }
      DomainName next_domain = ConvertLabels(r.nsec_next_labels);
      auto record = MdnsRecord::TryCreate(
          std::move(name), dns_type, dns_class, record_type, ttl,
          NsecRecordRdata(std::move(next_domain), std::move(types)));
      if (record.is_error()) {
        return Error::Code::kMdnsReadFailure;
      }
      return std::move(record.value());
    }
    default: {
      ByteView raw_view(r.rdata_bytes.data(), r.rdata_bytes.size());
      auto record =
          MdnsRecord::TryCreate(std::move(name), dns_type, dns_class,
                                record_type, ttl, RawRecordRdata(raw_view));
      if (record.is_error()) {
        return Error::Code::kMdnsReadFailure;
      }
      return std::move(record.value());
    }
  }
}

}  // namespace

// static
ErrorOr<MdnsMessage> SimpleMdnsReader::Read(const Config& config,
                                            ByteView buffer) {
  OSP_CHECK_GT(config.maximum_valid_rdata_size, 0);

  rust::Slice<const uint8_t> buffer_slice(buffer.data(), buffer.size());
  DnsMessage dns_msg;
  if (!parse_message(buffer_slice, dns_msg)) {
    return Error::Code::kMdnsReadFailure;
  }

  std::vector<MdnsQuestion> questions;
  questions.reserve(dns_msg.questions.size());
  for (const auto& q : dns_msg.questions) {
    auto question = ConvertQuestion(q);
    if (question.is_error()) {
      return question.error();
    }
    questions.push_back(std::move(question.value()));
  }

  const size_t max_allowed_rdata_size =
      static_cast<size_t>(config.maximum_valid_rdata_size);
  auto convert_records =
      [max_allowed_rdata_size](const rust::Vec<DnsRecord>& dns_records)
      -> ErrorOr<std::vector<MdnsRecord>> {
    std::vector<MdnsRecord> records;
    records.reserve(dns_records.size());
    for (const auto& r : dns_records) {
      auto record_or_error = ConvertRecord(r, max_allowed_rdata_size);
      if (record_or_error.is_error()) {
        return record_or_error.error();
      }
      records.push_back(std::move(record_or_error.value()));
    }
    return records;
  };

  auto answers = convert_records(dns_msg.answers);
  if (answers.is_error()) {
    return answers.error();
  }

  auto authority = convert_records(dns_msg.authority_records);
  if (authority.is_error()) {
    return authority.error();
  }

  auto additional = convert_records(dns_msg.additional_records);
  if (additional.is_error()) {
    return additional.error();
  }

  const MessageType msg_type =
      dns_msg.header.is_response ? MessageType::Response : MessageType::Query;

  ErrorOr<MdnsMessage> message = MdnsMessage::TryCreate(
      dns_msg.header.id, msg_type, std::move(questions),
      std::move(answers.value()), std::move(authority.value()),
      std::move(additional.value()));
  if (message.is_error()) {
    return message.error();
  }

  if (dns_msg.header.is_truncated) {
    message.value().set_truncated();
  }

  return std::move(message.value());
}

SimpleMdnsReader::SimpleMdnsReader(const Config& config, ByteView buffer)
    : config_(config), buffer_(buffer) {}

ErrorOr<MdnsMessage> SimpleMdnsReader::Read() const {
  return Read(config_, buffer_);
}

}  // namespace openscreen::discovery
