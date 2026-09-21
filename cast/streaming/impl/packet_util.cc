// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/streaming/impl/packet_util.h"

#include "cast/streaming/impl/rtcp_common.h"
#include "cast/streaming/impl/rtp_defines.h"

#if defined(USE_RUST_RTP_PARSER)
#include "cast/streaming/impl/rtp_wire.rs.h"
#endif  // defined(USE_RUST_RTP_PARSER)

namespace openscreen::cast {

std::pair<ApparentPacketType, Ssrc> InspectPacketForRouting(ByteView packet) {
#if defined(USE_RUST_RTP_PARSER)
  const rust::Slice<const uint8_t> slice(packet.data(), packet.size());
  const RoutingResult result = inspect_packet_for_routing(slice);
  switch (result.apparent_type) {
    case ApparentType::Rtp:
      return std::make_pair(ApparentPacketType::RTP, Ssrc{result.ssrc});
    case ApparentType::Rtcp:
      return std::make_pair(ApparentPacketType::RTCP, Ssrc{result.ssrc});
    default:
      return std::make_pair(ApparentPacketType::UNKNOWN, Ssrc{0});
  }
#else
  // Check for RTP packets first, since they are more frequent.
  if (packet.size() >= kRtpPacketMinValidSize &&
      packet[0] == kRtpRequiredFirstByte &&
      IsRtpPayloadType(packet[1] & kRtpPayloadTypeMask)) {
    constexpr int kOffsetToSsrcField = 8;
    return std::make_pair(
        ApparentPacketType::RTP,
        Ssrc{ReadBigEndian<uint32_t>(packet.subspan(kOffsetToSsrcField))});
  }

  // While RTCP packets are valid if they consist of just the RTCP Common
  // Header, all the RTCP packet types processed by this implementation will
  // also have a SSRC field immediately following the header. This is important
  // for routing the packet to the correct parser instance.
  constexpr int kRtcpPacketMinAcceptableSize =
      kRtcpCommonHeaderSize + sizeof(uint32_t);
  if (packet.size() >= kRtcpPacketMinAcceptableSize &&
      RtcpCommonHeader::Parse(packet).has_value()) {
    return std::make_pair(
        ApparentPacketType::RTCP,
        Ssrc{ReadBigEndian<uint32_t>(packet.subspan(kRtcpCommonHeaderSize))});
  }

  return std::make_pair(ApparentPacketType::UNKNOWN, Ssrc{0});
#endif  // defined(USE_RUST_RTP_PARSER)
}

}  // namespace openscreen::cast
