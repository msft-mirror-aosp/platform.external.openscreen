// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "discovery/mdns/public/simple_mdns_writer.h"

#include <string>
#include <utility>
#include <vector>

#include "discovery/mdns/public/mdns_constants.h"
#include "discovery/mdns/public/mdns_records.h"
#include "discovery/mdns/public/mdns_wire.rs.h"

namespace openscreen::discovery {
namespace {

rust::Vec<rust::String> ConvertLabels(const DomainName& name) {
  rust::Vec<rust::String> labels;
  labels.reserve(name.labels().size());
  for (const auto& label : name.labels()) {
    labels.push_back(rust::String(label));
  }
  return labels;
}

DnsRecord ConvertRecord(const MdnsRecord& r) {
  DnsRecord rr;
  rr.name_labels = ConvertLabels(r.name());
  rr.rtype = static_cast<uint16_t>(r.dns_type());
  rr.rclass = static_cast<uint16_t>(r.dns_class());
  rr.is_cache_flush = (r.record_type() == RecordType::kUnique);
  rr.ttl_seconds = static_cast<uint32_t>(r.ttl().count());

  switch (r.dns_type()) {
    case DnsType::kA: {
      const auto& a = std::get<ARecordRdata>(r.rdata());
      const auto bytes = a.ipv4_address().bytes();
      rr.rdata_bytes.reserve(bytes.size());
      for (uint8_t b : bytes) {
        rr.rdata_bytes.push_back(b);
      }
      break;
    }
    case DnsType::kAAAA: {
      const auto& aaaa = std::get<AAAARecordRdata>(r.rdata());
      const auto bytes = aaaa.ipv6_address().bytes();
      rr.rdata_bytes.reserve(bytes.size());
      for (uint8_t b : bytes) {
        rr.rdata_bytes.push_back(b);
      }
      break;
    }
    case DnsType::kPTR: {
      const auto& ptr = std::get<PtrRecordRdata>(r.rdata());
      rr.ptr_target_labels = ConvertLabels(ptr.ptr_domain());
      break;
    }
    case DnsType::kSRV: {
      const auto& srv = std::get<SrvRecordRdata>(r.rdata());
      rr.srv_priority = srv.priority();
      rr.srv_weight = srv.weight();
      rr.srv_port = srv.port();
      rr.srv_target_labels = ConvertLabels(srv.target());
      break;
    }
    case DnsType::kTXT: {
      const auto& txt = std::get<TxtRecordRdata>(r.rdata());
      for (const auto& entry : txt.texts()) {
        rr.rdata_bytes.push_back(static_cast<uint8_t>(entry.size()));
        for (uint8_t b : entry) {
          rr.rdata_bytes.push_back(b);
        }
      }
      break;
    }
    case DnsType::kNSEC: {
      const auto& nsec = std::get<NsecRecordRdata>(r.rdata());
      rr.nsec_next_labels = ConvertLabels(nsec.next_domain_name());
      for (uint8_t b : nsec.encoded_types()) {
        rr.rdata_bytes.push_back(b);
      }
      break;
    }
    default: {
      const auto& raw = std::get<RawRecordRdata>(r.rdata());
      for (size_t i = 0; i < raw.size(); ++i) {
        rr.rdata_bytes.push_back(raw.data()[i]);
      }
      break;
    }
  }
  return rr;
}

}  // namespace

// static
ErrorOr<std::vector<uint8_t>> SimpleMdnsWriter::Write(
    const MdnsMessage& message) {
  DnsMessage dns_msg;
  dns_msg.header.id = message.id();
  dns_msg.header.is_response = (message.type() == MessageType::Response);
  dns_msg.header.is_truncated = message.is_truncated();
  dns_msg.header.is_authoritative = (message.type() == MessageType::Response);

  dns_msg.questions.reserve(message.questions().size());
  for (const auto& q : message.questions()) {
    DnsQuestion dq;
    dq.name_labels = ConvertLabels(q.name());
    dq.qtype = static_cast<uint16_t>(q.dns_type());
    dq.qclass = static_cast<uint16_t>(q.dns_class());
    dq.is_unicast_response = (q.response_type() == ResponseType::kUnicast);
    dns_msg.questions.push_back(std::move(dq));
  }

  // TODO(crbug.com/554350196): Once simple-dns is fully stabilized and the
  // legacy C++ mDNS parser/writer are retired, investigate unifying MdnsRecord
  // and DnsRecord to eliminate this conversion step.
  auto append_records = [](const std::vector<MdnsRecord>& src,
                           rust::Vec<DnsRecord>& dest) {
    dest.reserve(src.size());
    for (const auto& r : src) {
      dest.push_back(ConvertRecord(r));
    }
  };
  append_records(message.answers(), dns_msg.answers);
  append_records(message.authority_records(), dns_msg.authority_records);
  append_records(message.additional_records(), dns_msg.additional_records);

  rust::Vec<uint8_t> out;
  if (!write_message(dns_msg, out)) {
    return Error::Code::kInsufficientBuffer;
  }

  return std::vector<uint8_t>(out.begin(), out.end());
}

}  // namespace openscreen::discovery
