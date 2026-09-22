// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/streaming/impl/rtp_packet_parser.h"

#include <algorithm>
#include <utility>

#include "cast/streaming/impl/packet_util.h"
#include "util/osp_logging.h"

#if defined(USE_RUST_RTP_PARSER)
#include "cast/streaming/impl/rtp_wire.rs.h"
#endif

namespace openscreen::cast {

RtpPacketParser::RtpPacketParser(Ssrc sender_ssrc, RtpParserVersion version)
    : sender_ssrc_(sender_ssrc),
      version_(version),
      highest_rtp_frame_id_(FrameId::first()) {}

RtpPacketParser::~RtpPacketParser() = default;

std::optional<RtpPacketParser::ParseResult> RtpPacketParser::Parse(
    ByteView buffer) {
#if defined(USE_RUST_RTP_PARSER)
  switch (version_) {
    case RtpParserVersion::kV1:
      return ParseV1(buffer);
    case RtpParserVersion::kV2:
      return ParseV2(buffer);
  }
#else
  return ParseV1(buffer);
#endif
}

#if defined(USE_RUST_RTP_PARSER)
// ParseV2: Memory-safe Rust wire parser implementation (via CXX FFI) in
// //cast/streaming/impl/rtp_wire.rs.
std::optional<RtpPacketParser::ParseResult> RtpPacketParser::ParseV2(
    ByteView buffer) {
  const rust::Slice<const uint8_t> slice(buffer.data(), buffer.size());
  WireRtpPacket wire;
  if (!parse_rtp_packet(slice, sender_ssrc_, wire)) {
    return std::nullopt;
  }
  ParseResult result;
  result.payload_type = static_cast<RtpPayloadType>(wire.payload_type);
  result.sequence_number = wire.sequence_number;
  result.rtp_timestamp =
      last_parsed_rtp_timestamp_.Expand(wire.truncated_rtp_timestamp);
  result.is_key_frame = wire.is_key_frame;
  result.frame_id = highest_rtp_frame_id_.Expand(wire.truncated_frame_id);
  result.packet_id = wire.packet_id;
  result.max_packet_id = wire.max_packet_id;
  if (wire.has_referenced_frame_id) {
    result.referenced_frame_id =
        result.frame_id.Expand(wire.truncated_referenced_frame_id);
  } else {
    result.referenced_frame_id =
        result.is_key_frame ? result.frame_id : (result.frame_id - 1);
  }
  if (wire.has_new_playout_delay) {
    result.new_playout_delay =
        std::chrono::milliseconds(wire.new_playout_delay_ms);
  }
  result.payload = buffer.subspan(wire.payload_offset, wire.payload_len);

  last_parsed_rtp_timestamp_ = result.rtp_timestamp;
  highest_rtp_frame_id_ = std::max(highest_rtp_frame_id_, result.frame_id);
  return result;
}
#endif  // defined(USE_RUST_RTP_PARSER)

// ParseV1: Original C++ wire parser implementation using ConsumeField<T>.
std::optional<RtpPacketParser::ParseResult> RtpPacketParser::ParseV1(
    ByteView buffer) {
  if (buffer.size() < kRtpPacketMinValidSize ||
      ConsumeField<uint8_t>(buffer) != kRtpRequiredFirstByte) {
    return std::nullopt;
  }

  // RTP header elements.
  //
  // Note: M (marker bit) is ignored here. Technically, according to the Cast
  // Streaming spec, it should only be set when PID == Max PID; but, let's be
  // lenient just in case some sender implementations don't adhere to this tiny,
  // subtle detail.
  const uint8_t payload_type =
      ConsumeField<uint8_t>(buffer) & kRtpPayloadTypeMask;
  if (!IsRtpPayloadType(payload_type)) {
    return std::nullopt;
  }
  ParseResult result;
  result.payload_type = static_cast<RtpPayloadType>(payload_type);
  result.sequence_number = ConsumeField<uint16_t>(buffer);
  result.rtp_timestamp =
      last_parsed_rtp_timestamp_.Expand(ConsumeField<uint32_t>(buffer));
  if (ConsumeField<uint32_t>(buffer) != sender_ssrc_) {
    return std::nullopt;
  }

  // Cast-specific header elements.
  const uint8_t byte12 = ConsumeField<uint8_t>(buffer);
  result.is_key_frame = !!(byte12 & kRtpKeyFrameBitMask);
  const bool has_referenced_frame_id =
      !!(byte12 & kRtpHasReferenceFrameIdBitMask);
  const size_t num_cast_extensions = byte12 & kRtpExtensionCountMask;
  result.frame_id = highest_rtp_frame_id_.Expand(ConsumeField<uint8_t>(buffer));
  result.packet_id = ConsumeField<uint16_t>(buffer);
  result.max_packet_id = ConsumeField<uint16_t>(buffer);
  if (result.max_packet_id == kAllPacketsLost) {
    return std::nullopt;  // Packet ID cannot be the special value.
  }
  if (result.packet_id > result.max_packet_id) {
    return std::nullopt;
  }
  if (has_referenced_frame_id) {
    if (buffer.empty()) {
      return std::nullopt;
    }
    result.referenced_frame_id =
        result.frame_id.Expand(ConsumeField<uint8_t>(buffer));
  } else {
    // By default, if no reference frame ID was provided, the assumption is that
    // a key frame only references itself, while non-key frames reference only
    // their immediate predecessor.
    result.referenced_frame_id =
        result.is_key_frame ? result.frame_id : (result.frame_id - 1);
  }

  // Zero or more Cast extensions.
  for (size_t i = 0; i < num_cast_extensions; ++i) {
    if (buffer.size() < sizeof(uint16_t)) {
      return std::nullopt;
    }
    const uint16_t type_and_size = ConsumeField<uint16_t>(buffer);
    const uint8_t type = type_and_size >> kNumExtensionDataSizeFieldBits;
    const size_t size =
        type_and_size & FieldBitmask<uint16_t>(kNumExtensionDataSizeFieldBits);
    if (buffer.size() < size) {
      return std::nullopt;
    }
    if (type == kAdaptiveLatencyRtpExtensionType) {
      if (size != sizeof(uint16_t)) {
        return std::nullopt;
      }
      result.new_playout_delay =
          std::chrono::milliseconds(ReadBigEndian<uint16_t>(buffer));
    }
    buffer = buffer.subspan(size);
  }

  // All remaining data in the packet is the payload.
  result.payload = buffer;

  // At this point, the packet is known to be well-formed. Track recent field
  // values for later parses, to bit-extend the truncated values found in future
  // packets.
  last_parsed_rtp_timestamp_ = result.rtp_timestamp;
  highest_rtp_frame_id_ = std::max(highest_rtp_frame_id_, result.frame_id);

  return result;
}

RtpPacketParser::ParseResult::ParseResult() = default;
RtpPacketParser::ParseResult::~ParseResult() = default;

}  // namespace openscreen::cast
