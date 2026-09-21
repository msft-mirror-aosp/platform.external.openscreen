// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <vector>

#include "cast/streaming/impl/rtp_wire.rs.h"
#include "gtest/gtest.h"

namespace openscreen::cast {
static_assert(USE_RUST_RTP_PARSER);

namespace {

constexpr uint32_t kSenderSsrc = 0x11223344;
constexpr uint32_t kReceiverSsrc = 0x55667788;

TEST(RtpWireTest, InspectPacketForRoutingRtp) {
  // Minimal 18-byte Cast RTP packet:
  // Byte 0: V=2 (0x80)
  // Byte 1: PT=96 (0x60 - Audio Opus)
  // Bytes 2-3: Seq num = 1
  // Bytes 4-7: Timestamp = 1000
  // Bytes 8-11: SSRC = 0x11223344
  // Bytes 12-17: Keyframe, FID=1, PID=0, MaxPID=0
  const uint8_t kRtpPacket[] = {
      0x80, 0x60, 0x00, 0x01,  // V=2, PT=96, seq=1
      0x00, 0x00, 0x03, 0xe8,  // timestamp=1000
      0x11, 0x22, 0x33, 0x44,  // SSRC = 0x11223344
      0x80, 0x01, 0x00, 0x00,
      0x00, 0x00,  // key=1, ext=0, FID=1, PID=0, MaxPID=0
  };

  const rust::Slice<const uint8_t> slice(kRtpPacket, sizeof(kRtpPacket));
  const RoutingResult result = inspect_packet_for_routing(slice);
  EXPECT_EQ(result.apparent_type, ApparentType::Rtp);
  EXPECT_EQ(result.ssrc, kSenderSsrc);
}

TEST(RtpWireTest, InspectPacketForRoutingRtcp) {
  // RTCP Common Header + SSRC (8 bytes):
  // Byte 0: V=2, RC=0 (0x80)
  // Byte 1: PT=200 (Sender Report)
  // Bytes 2-3: Length in 32-bit words - 1 = 6 (28 bytes total)
  // Bytes 4-7: Sender SSRC = 0x11223344
  const uint8_t kRtcpPacket[] = {
      0x80, 0xc8, 0x00, 0x06,  // V=2, PT=200, length=6 words (28 bytes)
      0x11, 0x22, 0x33, 0x44,  // SSRC = 0x11223344
  };

  const rust::Slice<const uint8_t> slice(kRtcpPacket, sizeof(kRtcpPacket));
  const RoutingResult result = inspect_packet_for_routing(slice);
  EXPECT_EQ(result.apparent_type, ApparentType::Rtcp);
  EXPECT_EQ(result.ssrc, kSenderSsrc);
}

TEST(RtpWireTest, InspectPacketForRoutingMalformed) {
  const uint8_t kGarbage[] = {0x00, 0x01, 0x02, 0x03};
  const rust::Slice<const uint8_t> slice(kGarbage, sizeof(kGarbage));
  const RoutingResult result = inspect_packet_for_routing(slice);
  EXPECT_EQ(result.apparent_type, ApparentType::Unknown);
  EXPECT_EQ(result.ssrc, 0u);
}

TEST(RtpWireTest, ParseRtcpCommonHeaderValid) {
  // RTCP Sender Report with 1 report block (28 bytes payload):
  // Byte 0: V=2, RC=1 -> 0x81
  // Byte 1: PT=200 -> 0xc8
  // Bytes 2-3: Word count = 7 -> 0x0007 (payload size = 28 bytes)
  const uint8_t kHeaderBytes[] = {0x81, 0xc8, 0x00, 0x07};
  const rust::Slice<const uint8_t> slice(kHeaderBytes, sizeof(kHeaderBytes));

  WireRtcpCommonHeader header;
  EXPECT_TRUE(parse_rtcp_common_header(slice, header));
  EXPECT_EQ(header.packet_type, 200);
  EXPECT_EQ(header.report_count_or_subtype, 1);
  EXPECT_EQ(header.payload_size, 28u);
}

TEST(RtpWireTest, ParseRtcpCommonHeaderInvalidVersion) {
  // Invalid version (V=1 instead of V=2)
  const uint8_t kBadHeader[] = {0x41, 0xc8, 0x00, 0x07};
  const rust::Slice<const uint8_t> slice(kBadHeader, sizeof(kBadHeader));

  WireRtcpCommonHeader header;
  EXPECT_FALSE(parse_rtcp_common_header(slice, header));
}

TEST(RtpWireTest, ParseSenderReportPacketFull) {
  // Compound RTCP packet containing a Sender Report with 1 Report Block:
  // Common Header (4 bytes): V=2, RC=1, PT=200, length=7 (28 bytes)
  // Sender SSRC (4 bytes): 0x11223344
  // NTP timestamp (8 bytes): 0x00010000_00000000
  // RTP timestamp (4 bytes): 90000 (0x00015f90)
  // Packet count (4 bytes): 100
  // Octet count (4 bytes): 50000
  // Report Block for receiver 0x55667788 (24 bytes):
  //   SSRC (4 bytes): 0x55667788
  //   Fraction lost (1 byte) = 5, Cumulative lost (3 bytes) = 10 -> 0x0500000a
  //   Extended highest seq (4 bytes) = 1500
  //   Jitter (4 bytes) = 20
  //   LSR (4 bytes) = 0x12345678
  //   DLSR (4 bytes) = 0x00010000
  const uint8_t kPacket[] = {
      0x81, 0xc8,
      0x00, 0x0c,  // Header: PT=200, RC=1, length=12 words (48 bytes payload)
      0x11, 0x22,
      0x33, 0x44,  // Sender SSRC
      0x00, 0x01,
      0x00, 0x00,  // NTP MSW
      0x00, 0x00,
      0x00, 0x00,  // NTP LSW
      0x00, 0x01,
      0x5f, 0x90,  // RTP timestamp = 90000
      0x00, 0x00,
      0x00, 0x64,  // Send packet count = 100
      0x00, 0x00,
      0xc3, 0x50,  // Send octet count = 50000
      0x55, 0x66,
      0x77, 0x88,  // Report Block SSRC = 0x55667788
      0x05, 0x00,
      0x00, 0x0a,  // Fraction=5, Cumulative=10
      0x00, 0x00,
      0x05, 0xdc,  // Extended seq = 1500
      0x00, 0x00,
      0x00, 0x14,  // Jitter = 20 ticks
      0x12, 0x34,
      0x56, 0x78,  // LSR = 0x12345678
      0x00, 0x01,
      0x00, 0x00,  // DLSR = 65536 ticks (1 sec)
  };

  const rust::Slice<const uint8_t> slice(kPacket, sizeof(kPacket));
  WireSenderReport report;
  EXPECT_TRUE(
      parse_sender_report_packet(slice, kSenderSsrc, kReceiverSsrc, report));
  EXPECT_TRUE(report.has_sender_report);
  EXPECT_EQ(report.ntp_timestamp, 0x0001000000000000ULL);
  EXPECT_EQ(report.truncated_rtp_timestamp, 90000u);
  EXPECT_EQ(report.send_packet_count, 100u);
  EXPECT_EQ(report.send_octet_count, 50000u);

  EXPECT_TRUE(report.has_report_block);
  EXPECT_EQ(report.report_block.ssrc, kReceiverSsrc);
  EXPECT_EQ(report.report_block.packet_fraction_lost_numerator, 5);
  EXPECT_EQ(report.report_block.cumulative_packets_lost, 10);
  EXPECT_EQ(report.report_block.extended_high_sequence_number, 1500u);
  EXPECT_EQ(report.report_block.jitter_ticks, 20u);
  EXPECT_EQ(report.report_block.last_status_report_id, 0x12345678u);
  EXPECT_EQ(report.report_block.delay_since_last_report_ticks, 65536u);
}

TEST(RtpWireTest, ParseRtpPacketWithAdaptiveLatencyExtension) {
  // Cast RTP packet with Adaptive Latency Extension:
  // Byte 0: 0x80 (V=2)
  // Byte 1: 0x64 (PT=100 - Video VP8)
  // Bytes 2-3: Seq = 42
  // Bytes 4-7: Timestamp = 180000
  // Bytes 8-11: Sender SSRC = 0x11223344
  // Byte 12: Key=1, Ref=0, ExtCount=1 -> 0x81
  // Byte 13: Frame ID = 5
  // Bytes 14-15: Packet ID = 0
  // Bytes 16-17: Max Packet ID = 3
  // Extension Word 1 (4 bytes): Type=1 (Adaptive Latency, 6 bits), Size=2 (10
  // bits) -> 0x0402
  //   Playout Delay = 400ms -> 0x0190
  // Payload (4 bytes): 0xde 0xad 0xbe 0xef
  const uint8_t kRtpPacket[] = {
      0x80, 0x64, 0x00, 0x2a,  // V=2, PT=100, seq=42
      0x00, 0x02, 0xbf, 0x20,  // timestamp = 180000
      0x11, 0x22, 0x33, 0x44,  // SSRC = 0x11223344
      0x81, 0x05, 0x00, 0x00,
      0x00, 0x03,              // key=1, ext=1, FID=5, PID=0, MaxPID=3
      0x04, 0x02, 0x01, 0x90,  // Ext: Type=1, Size=2, Delay=400ms
      0xde, 0xad, 0xbe, 0xef,  // Payload
  };

  const rust::Slice<const uint8_t> slice(kRtpPacket, sizeof(kRtpPacket));
  WireRtpPacket packet;
  EXPECT_TRUE(parse_rtp_packet(slice, kSenderSsrc, packet));
  EXPECT_EQ(packet.payload_type, 100);
  EXPECT_EQ(packet.sequence_number, 42);
  EXPECT_EQ(packet.truncated_rtp_timestamp, 180000u);
  EXPECT_TRUE(packet.is_key_frame);
  EXPECT_FALSE(packet.has_referenced_frame_id);
  EXPECT_EQ(packet.truncated_frame_id, 5);
  EXPECT_EQ(packet.packet_id, 0);
  EXPECT_EQ(packet.max_packet_id, 3);
  EXPECT_TRUE(packet.has_new_playout_delay);
  EXPECT_EQ(packet.new_playout_delay_ms, 400);
  EXPECT_EQ(packet.payload_offset, 22u);
  EXPECT_EQ(packet.payload_len, 4u);
}

TEST(RtpWireTest, ParseCompoundRtcpFeedbackWithNacksAndAcks) {
  // Compound RTCP Packet with Cast Feedback:
  // Common Header (4 bytes): V=2, Subtype=15 (Feedback), PT=206, Length=7 words
  // (28 bytes payload) Receiver SSRC (4 bytes): 0x55667788 Sender SSRC (4
  // bytes): 0x11223344 CAST magic (4 bytes): 'C' 'A' 'S' 'T' (0x43415354)
  // Checkpoint FID (1 byte): 10
  // Loss count (1 byte): 1
  // Current Playout Delay (2 bytes): 250ms (0x00fa)
  // Loss Field 1 (4 bytes): Within FID=11, Lost PID=2, Bitmask=0x01 (PID 3
  // missing) Followed by CST2 ACK block (8 bytes):
  //   CST2 magic (4 bytes): 'C' 'S' 'T' '2' (0x43535432)
  //   Feedback count (1 byte): 1
  //   ACK bytes count (1 byte): 2
  //   ACK bits (2 bytes): 0x05 (Frame 12 & 14 received)
  const uint8_t kFeedbackPacket[] = {
      0x8f, 0xce,
      0x00, 0x07,  // Header: Subtype=15, PT=206, Length=7 words (28 bytes)
      0x55, 0x66,
      0x77, 0x88,  // Receiver SSRC
      0x11, 0x22,
      0x33, 0x44,  // Sender SSRC
      'C',  'A',
      'S',  'T',  // CAST magic
      0x0a, 0x01,
      0x00, 0xfa,  // Checkpoint FID=10, Loss=1, Delay=250ms
      0x0b, 0x00,
      0x02, 0x01,  // Within FID=11, PID=2, mask=1 (PID 3)
      'C',  'S',
      'T',  '2',  // CST2 magic
      0x01, 0x02,
      0x05, 0x00,  // Count=1, Bytes=2, Bitmask=0x0005
  };

  const rust::Slice<const uint8_t> slice(kFeedbackPacket,
                                         sizeof(kFeedbackPacket));
  WireCompoundRtcp out;
  EXPECT_TRUE(parse_compound_rtcp(slice, kReceiverSsrc, kSenderSsrc, 10, out));
  EXPECT_TRUE(out.has_checkpoint_frame_id);
  EXPECT_EQ(out.checkpoint_frame_id, 10);
  EXPECT_EQ(out.target_playout_delay_ms, 250);

  // Check NACKs
  ASSERT_EQ(out.packet_nacks.size(), 2u);
  EXPECT_EQ(out.packet_nacks[0].frame_id, 11);
  EXPECT_EQ(out.packet_nacks[0].packet_id, 2);
  EXPECT_EQ(out.packet_nacks[1].frame_id, 11);
  EXPECT_EQ(out.packet_nacks[1].packet_id, 3);

  // Check ACKs (Frame 12 and 14 from bitmask 0x05 starting at 10 + 2 = 12)
  ASSERT_EQ(out.received_frames.size(), 2u);
  EXPECT_EQ(out.received_frames[0], 12);
  EXPECT_EQ(out.received_frames[1], 14);
}

}  // namespace
}  // namespace openscreen::cast
