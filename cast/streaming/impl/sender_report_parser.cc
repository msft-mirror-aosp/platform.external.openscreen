// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/streaming/impl/sender_report_parser.h"

#include "cast/streaming/impl/packet_util.h"
#include "util/osp_logging.h"

#if defined(USE_RUST_RTP_PARSER)
#include "cast/streaming/impl/rtp_wire.rs.h"
#endif  // defined(USE_RUST_RTP_PARSER)

namespace openscreen::cast {

SenderReportParser::SenderReportWithId::SenderReportWithId() = default;
SenderReportParser::SenderReportWithId::~SenderReportWithId() = default;

SenderReportParser::SenderReportParser(RtcpSession& session)
    : session_(session) {}

SenderReportParser::~SenderReportParser() = default;

std::optional<SenderReportParser::SenderReportWithId> SenderReportParser::Parse(
    ByteView buffer) {
#if defined(USE_RUST_RTP_PARSER)
  return ParseV2(buffer);
#else
  return ParseV1(buffer);
#endif  // defined(USE_RUST_RTP_PARSER)
}

#if defined(USE_RUST_RTP_PARSER)
// ParseV2: Memory-safe Rust wire parser implementation (via CXX FFI) in
// //cast/streaming/impl/rtp_wire.rs.
std::optional<SenderReportParser::SenderReportWithId>
SenderReportParser::ParseV2(ByteView buffer) {
  const rust::Slice<const uint8_t> slice(buffer.data(), buffer.size());
  WireSenderReport wire;
  if (!parse_sender_report_packet(slice, session_->sender_ssrc(),
                                  session_->receiver_ssrc(), wire)) {
    return std::nullopt;
  }
  if (!wire.has_sender_report) {
    return std::nullopt;
  }
  SenderReportWithId report;
  // wire.ntp_timestamp is uint64_t matching NtpTimestamp.
  report.report_id = ToStatusReportId(wire.ntp_timestamp);
  report.reference_time =
      session_->ntp_converter().ToLocalTime(wire.ntp_timestamp);
  // wire.truncated_rtp_timestamp is uint32_t matching RTP timestamp wire width.
  report.rtp_timestamp =
      last_parsed_rtp_timestamp_.Expand(wire.truncated_rtp_timestamp);
  report.send_packet_count = wire.send_packet_count;
  report.send_octet_count = wire.send_octet_count;
  if (wire.has_report_block) {
    RtcpReportBlock rb;
    rb.ssrc = wire.report_block.ssrc;
    // packet_fraction_lost_numerator is an 8-bit uint [0, 255] stored in
    // i32/int.
    rb.packet_fraction_lost_numerator =
        wire.report_block.packet_fraction_lost_numerator;
    // cumulative_packets_lost is a 24-bit uint [0, 16777215] stored in i32/int.
    rb.cumulative_packets_lost = wire.report_block.cumulative_packets_lost;
    rb.extended_high_sequence_number =
        wire.report_block.extended_high_sequence_number;
    rb.jitter = RtpTimeDelta::FromTicks(wire.report_block.jitter_ticks);
    rb.last_status_report_id = wire.report_block.last_status_report_id;
    rb.delay_since_last_report =
        RtcpReportBlock::Delay(wire.report_block.delay_since_last_report_ticks);
    report.report_block = rb;
  }
  last_parsed_rtp_timestamp_ = report.rtp_timestamp;
  return report;
}
#else
// ParseV1: Original C++ wire parser implementation. Walks compound RTCP
// packets sequentially using field-by-field stream consumption.
std::optional<SenderReportParser::SenderReportWithId>
SenderReportParser::ParseV1(ByteView buffer) {
  std::optional<SenderReportWithId> sender_report;

  // The data contained in `buffer` can be a "compound packet," which means that
  // it can be the concatenation of multiple RTCP packets. The loop here
  // processes each one-by-one.
  while (!buffer.empty()) {
    const auto header = RtcpCommonHeader::Parse(buffer);
    if (!header) {
      return std::nullopt;
    }
    buffer = buffer.subspan(kRtcpCommonHeaderSize);
    if (static_cast<int>(buffer.size()) < header->payload_size) {
      return std::nullopt;
    }
    auto chunk = buffer.subspan(0, header->payload_size);
    buffer = buffer.subspan(header->payload_size);

    // Only process Sender Reports with a matching SSRC.
    if (header->packet_type != RtcpPacketType::kSenderReport) {
      continue;
    }
    if (header->payload_size < kRtcpSenderReportSize) {
      return std::nullopt;
    }
    if (ConsumeField<uint32_t>(chunk) != session_->sender_ssrc()) {
      continue;
    }
    SenderReportWithId& report = sender_report.emplace();
    const NtpTimestamp ntp_timestamp = ConsumeField<uint64_t>(chunk);
    report.report_id = ToStatusReportId(ntp_timestamp);
    report.reference_time =
        session_->ntp_converter().ToLocalTime(ntp_timestamp);
    report.rtp_timestamp =
        last_parsed_rtp_timestamp_.Expand(ConsumeField<uint32_t>(chunk));
    report.send_packet_count = ConsumeField<uint32_t>(chunk);
    report.send_octet_count = ConsumeField<uint32_t>(chunk);
    report.report_block = RtcpReportBlock::ParseOne(
        chunk, header->with.report_count, session_->receiver_ssrc());
  }

  // At this point, the packet is known to be well-formed. Cache the
  // most-recently parsed RTP timestamp value for bit-expansion in future
  // parses.
  if (sender_report) {
    last_parsed_rtp_timestamp_ = sender_report->rtp_timestamp;
  }
  return sender_report;
}
#endif  // defined(USE_RUST_RTP_PARSER)

}  // namespace openscreen::cast
