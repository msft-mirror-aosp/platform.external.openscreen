// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Cast Streaming wire format parser.
//!
//! Cast Streaming uses a specialized subset of RFC 3550 RTP and RTCP with
//! custom extensions, including:
//! - Cast RTP headers with custom truncated frame IDs and bitfield flags
//! - Adaptive latency extensions
//! - Custom RTCP feedback messages (CAST/CST2 ACK and loss NACK bitvectors)
//! - Receiver event log messages
//!
//! Because no public crate supports Cast's specialized wire formats and
//! bit-packing rules, this module provides a panic-free wire parser using
//! safe Rust slice operations and CXX FFI bindings.

// ============================================================================
// CXX FFI Declarations
// ============================================================================

#[cxx::bridge(namespace = "openscreen::cast")]
pub mod ffi {
    /// Apparent packet routing type.
    #[derive(Copy, Clone, PartialEq, Eq, Debug)]
    #[repr(u8)]
    pub enum ApparentType {
        Unknown = 0,
        Rtp = 1,
        Rtcp = 2,
    }

    #[derive(Debug, Default, Copy, Clone, PartialEq, Eq)]
    pub struct RoutingResult {
        pub apparent_type: ApparentType,
        /// Synchronization Source (SSRC) identifier per RFC 3550.
        pub ssrc: u32,
    }

    #[derive(Debug, Default, Copy, Clone, PartialEq, Eq)]
    pub struct WireRtcpCommonHeader {
        pub packet_type: u8,
        pub report_count_or_subtype: u8,
        pub payload_size: usize,
    }

    #[derive(Debug, Default, Copy, Clone, PartialEq, Eq)]
    pub struct WireRtcpReportBlock {
        /// Synchronization Source (SSRC) identifier per RFC 3550.
        pub ssrc: u32,
        pub packet_fraction_lost_numerator: i32,
        pub cumulative_packets_lost: i32,
        pub extended_high_sequence_number: u32,
        pub jitter_ticks: u32,
        pub last_status_report_id: u32,
        pub delay_since_last_report_ticks: u32,
    }

    #[derive(Debug, Default, Copy, Clone, PartialEq, Eq)]
    pub struct WireSenderReport {
        pub has_sender_report: bool,
        pub ntp_timestamp: u64,
        pub truncated_rtp_timestamp: u32,
        pub send_packet_count: u32,
        pub send_octet_count: u32,
        pub has_report_block: bool,
        pub report_block: WireRtcpReportBlock,
    }

    #[derive(Debug, Default, Copy, Clone, PartialEq, Eq)]
    pub struct WireRtpPacket {
        pub payload_type: u8,
        pub sequence_number: u16,
        pub truncated_rtp_timestamp: u32,
        pub is_key_frame: bool,
        pub truncated_frame_id: u8,
        pub packet_id: u16,
        pub max_packet_id: u16,
        pub has_referenced_frame_id: bool,
        pub truncated_referenced_frame_id: u8,
        pub has_new_playout_delay: bool,
        pub new_playout_delay_ms: u16,
        pub payload_offset: usize,
        pub payload_len: usize,
    }

    #[derive(Debug, Default, Copy, Clone, PartialEq, Eq)]
    pub struct WirePacketNack {
        pub frame_id: i64,
        pub packet_id: u16,
    }

    #[derive(Debug, Default, Copy, Clone, PartialEq, Eq)]
    pub struct WireReceiverEventLog {
        pub wire_type: u8,
        pub timestamp_delta_ms: u16,
        pub delay_delta_or_packet_id: u16,
    }

    #[derive(Debug, Default, Clone, PartialEq, Eq)]
    pub struct WireReceiverFrameLogMessage {
        pub truncated_rtp_timestamp: u32,
        pub raw_timestamp_ms: u32,
        pub events: Vec<WireReceiverEventLog>,
    }

    #[derive(Debug, Default, Clone, PartialEq, Eq)]
    pub struct WireCompoundRtcp {
        pub has_receiver_reference_ntp_time: bool,
        pub receiver_reference_ntp_time: u64,
        pub has_receiver_report: bool,
        pub receiver_report: WireRtcpReportBlock,
        pub log_messages: Vec<WireReceiverFrameLogMessage>,
        pub has_checkpoint_frame_id: bool,
        pub checkpoint_frame_id: i64,
        pub target_playout_delay_ms: u16,
        pub received_frames: Vec<i64>,
        pub packet_nacks: Vec<WirePacketNack>,
        pub picture_loss_indicator: bool,
    }

    extern "Rust" {
        fn inspect_packet_for_routing(packet: &[u8]) -> RoutingResult;
        fn parse_rtcp_common_header(buffer: &[u8], out: &mut WireRtcpCommonHeader) -> bool;
        fn parse_rtcp_report_block(
            buffer: &[u8],
            report_count: usize,
            target_ssrc: u32,
            out: &mut WireRtcpReportBlock,
        ) -> bool;
        fn parse_sender_report_packet(
            buffer: &[u8],
            sender_ssrc: u32,
            receiver_ssrc: u32,
            out: &mut WireSenderReport,
        ) -> bool;
        fn parse_rtp_packet(buffer: &[u8], sender_ssrc: u32, out: &mut WireRtpPacket) -> bool;
        fn parse_compound_rtcp(
            buffer: &[u8],
            receiver_ssrc: u32,
            sender_ssrc: u32,
            max_feedback_frame_id: i64,
            out: &mut WireCompoundRtcp,
        ) -> bool;
    }
}

// ============================================================================
// Wire Format Constants
// ============================================================================

const RTP_PACKET_MIN_VALID_SIZE: usize = 18;
const RTP_REQUIRED_FIRST_BYTE: u8 = 0b1000_0000;
const RTP_PAYLOAD_TYPE_MASK: u8 = 0b0111_1111;
const RTP_KEY_FRAME_BIT_MASK: u8 = 0b1000_0000;
const RTP_HAS_REF_FRAME_ID_BIT_MASK: u8 = 0b0100_0000;
const RTP_EXTENSION_COUNT_MASK: u8 = 0b0011_1111;
const ADAPTIVE_LATENCY_RTP_EXTENSION_TYPE: u8 = 1;
const ALL_PACKETS_LOST: u16 = 0xffff;

const RTCP_COMMON_HEADER_SIZE: usize = 4;
const RTCP_REQUIRED_VERSION_AND_PADDING_BITS: u8 = 0b100;
const RTCP_REPORT_BLOCK_SIZE: usize = 24;
const RTCP_SENDER_REPORT_SIZE: usize = 24;
const RTCP_RECEIVER_REPORT_SIZE: usize = 4;
const RTCP_FEEDBACK_HEADER_SIZE: usize = 16;
const RTCP_FEEDBACK_LOSS_FIELD_SIZE: usize = 4;
const RTCP_FEEDBACK_ACK_HEADER_SIZE: usize = 6;
const RTCP_EXTENDED_REPORT_HEADER_SIZE: usize = 4;
const RTCP_RECEIVER_REF_TIME_REPORT_BLOCK_TYPE: u8 = 4;
const RTCP_PICTURE_LOSS_INDICATOR_HEADER_SIZE: usize = 8;

const CAST_IDENTIFIER_WORD: u32 = u32::from_be_bytes(*b"CAST");
const CST2_IDENTIFIER_WORD: u32 = u32::from_be_bytes(*b"CST2");
const TIME_SYNC_REQUEST_NAME: u32 = u32::from_be_bytes(*b"TIME");

use strum::FromRepr;

// ============================================================================
// Wire Enums
// ============================================================================

impl Default for ffi::ApparentType {
    fn default() -> Self {
        Self::Unknown
    }
}

/// RTCP packet type constants (RFC 3550 & RFC 4585).
#[derive(Copy, Clone, PartialEq, Eq, Debug, FromRepr)]
#[repr(u8)]
pub enum RtcpPacketType {
    SenderReport = 200,
    ReceiverReport = 201,
    SourceDescription = 202,
    ApplicationDefined = 204,
    PayloadSpecific = 206,
    ExtendedReports = 207,
}

/// Cast-specific RTCP message subtypes.
#[derive(Copy, Clone, PartialEq, Eq, Debug, FromRepr)]
#[repr(u8)]
pub enum RtcpSubtype {
    PictureLossIndicator = 1,
    ReceiverLog = 2,
    Feedback = 15,
}

// ============================================================================
// Zero-Copy Panic-Free Byte Slice Reader
// ============================================================================

/// Zero-copy, panic-free byte slice cursor for wire-format parsing.
///
/// Note: While `std::io::Cursor<&[u8]>` provides standard stream-oriented
/// seeking and reading, it lacks built-in big-endian primitive extraction
/// and zero-copy sub-slice borrowing (`&'a [u8]`) without custom extension
/// traits. `Reader` is a lightweight, self-contained slice cursor tailored
/// specifically for packet parsing with native `usize` offsets, checked
/// big-endian decoding, and bounded `sub_reader`s.
#[derive(Clone, Copy)]
struct Reader<'a> {
    data: &'a [u8],
    offset: usize,
}

impl<'a> Reader<'a> {
    #[inline]
    const fn new(data: &'a [u8]) -> Self {
        Self { data, offset: 0 }
    }

    #[inline]
    const fn remaining(&self) -> usize {
        self.data.len().saturating_sub(self.offset)
    }

    #[inline]
    const fn is_empty(&self) -> bool {
        self.remaining() == 0
    }

    #[inline]
    const fn offset(&self) -> usize {
        self.offset
    }

    #[inline]
    const fn peek_u8(&self) -> Option<u8> {
        if self.offset < self.data.len() {
            Some(self.data[self.offset])
        } else {
            None
        }
    }

    #[inline]
    fn read_u8(&mut self) -> Option<u8> {
        let &b = self.data.get(self.offset)?;
        self.offset += 1;
        Some(b)
    }

    #[inline]
    fn read_slice(&mut self, len: usize) -> Option<&'a [u8]> {
        let end = self.offset.checked_add(len)?;
        let slice = self.data.get(self.offset..end)?;
        self.offset = end;
        Some(slice)
    }

    #[inline]
    fn read_array<const N: usize>(&mut self) -> Option<[u8; N]> {
        let slice = self.read_slice(N)?;
        slice.try_into().ok()
    }

    #[inline]
    fn read_u16_be(&mut self) -> Option<u16> {
        self.read_array().map(u16::from_be_bytes)
    }

    #[inline]
    fn read_u32_be(&mut self) -> Option<u32> {
        self.read_array().map(u32::from_be_bytes)
    }

    #[inline]
    fn read_u64_be(&mut self) -> Option<u64> {
        self.read_array().map(u64::from_be_bytes)
    }

    #[inline]
    fn sub_reader(&mut self, len: usize) -> Option<Reader<'a>> {
        let slice = self.read_slice(len)?;
        Some(Reader::new(slice))
    }
}

// ============================================================================
// Helper Utilities & Bit Expansions
// ============================================================================

#[inline]
fn is_rtp_payload_type(raw_byte: u8) -> bool {
    matches!(raw_byte, 96..=104 | 127)
}

#[inline]
fn expand_less_than_or_equal_u8(value: i64, x: u8) -> i64 {
    const SHORT_MAX: i64 = u8::MAX as i64;
    let mut result = (value & !SHORT_MAX) | (x as i64);
    if result > value {
        result = result.wrapping_sub(SHORT_MAX + 1);
    }
    result
}

#[inline]
fn expand_greater_than_u8(value: i64, x: u8) -> i64 {
    let max_possible = value.wrapping_add((u8::MAX as i64) + 1);
    expand_less_than_or_equal_u8(max_possible, x)
}

// ============================================================================
// RTCP Common Header & Routing Inspection
// ============================================================================

fn parse_common_header(reader: &mut Reader<'_>) -> Option<ffi::WireRtcpCommonHeader> {
    let byte0 = reader.read_u8()?;
    if (byte0 >> 5) != RTCP_REQUIRED_VERSION_AND_PADDING_BITS {
        return None;
    }
    let raw_report_count_or_subtype = byte0 & 0x1F;
    let byte1 = reader.read_u8()?;
    let packet_type = RtcpPacketType::from_repr(byte1)?;

    let report_count_or_subtype = match packet_type {
        RtcpPacketType::SenderReport | RtcpPacketType::ReceiverReport => {
            raw_report_count_or_subtype
        }
        RtcpPacketType::ApplicationDefined | RtcpPacketType::PayloadSpecific => {
            if RtcpSubtype::from_repr(raw_report_count_or_subtype).is_some() {
                raw_report_count_or_subtype
            } else {
                0
            }
        }
        _ => 0,
    };

    let word_count = reader.read_u16_be()? as usize;
    let payload_size = word_count.checked_mul(4)?;

    Some(ffi::WireRtcpCommonHeader { packet_type: byte1, report_count_or_subtype, payload_size })
}

pub fn parse_rtcp_common_header(buffer: &[u8], out: &mut ffi::WireRtcpCommonHeader) -> bool {
    let mut reader = Reader::new(buffer);
    if let Some(hdr) = parse_common_header(&mut reader) {
        *out = hdr;
        true
    } else {
        false
    }
}

pub fn inspect_packet_for_routing(packet: &[u8]) -> ffi::RoutingResult {
    let mut reader = Reader::new(packet);
    if reader.remaining() >= RTP_PACKET_MIN_VALID_SIZE {
        if let Some(first_byte) = reader.peek_u8() {
            if first_byte == RTP_REQUIRED_FIRST_BYTE {
                let second_byte = packet[1];
                if is_rtp_payload_type(second_byte & RTP_PAYLOAD_TYPE_MASK) {
                    let _ = reader.read_slice(8);
                    if let Some(ssrc) = reader.read_u32_be() {
                        return ffi::RoutingResult { apparent_type: ffi::ApparentType::Rtp, ssrc };
                    }
                }
            }
        }
    }

    let mut common_header = ffi::WireRtcpCommonHeader::default();
    if packet.len() >= RTCP_COMMON_HEADER_SIZE + 4
        && parse_rtcp_common_header(&packet[..RTCP_COMMON_HEADER_SIZE], &mut common_header)
    {
        let mut rtcp_reader = Reader::new(&packet[RTCP_COMMON_HEADER_SIZE..]);
        if let Some(ssrc) = rtcp_reader.read_u32_be() {
            return ffi::RoutingResult { apparent_type: ffi::ApparentType::Rtcp, ssrc };
        }
    }

    ffi::RoutingResult { apparent_type: ffi::ApparentType::Unknown, ssrc: 0 }
}

// ============================================================================
// RTCP Report Block & Sender Report Parsing
// ============================================================================

fn parse_single_report_block(reader: &mut Reader<'_>) -> Option<ffi::WireRtcpReportBlock> {
    let ssrc = reader.read_u32_be()?;
    let second_word = reader.read_u32_be()?;
    let extended_high_sequence_number = reader.read_u32_be()?;
    let jitter_ticks = reader.read_u32_be()?;
    let last_status_report_id = reader.read_u32_be()?;
    let delay_since_last_report_ticks = reader.read_u32_be()?;

    Some(ffi::WireRtcpReportBlock {
        ssrc,
        packet_fraction_lost_numerator: (second_word >> 24) as i32,
        cumulative_packets_lost: (second_word & 0x00FF_FFFF) as i32,
        extended_high_sequence_number,
        jitter_ticks,
        last_status_report_id,
        delay_since_last_report_ticks,
    })
}

pub fn parse_rtcp_report_block(
    buffer: &[u8],
    report_count: usize,
    target_ssrc: u32,
    out: &mut ffi::WireRtcpReportBlock,
) -> bool {
    let Some(required_len) = report_count.checked_mul(RTCP_REPORT_BLOCK_SIZE) else {
        return false;
    };
    if buffer.len() < required_len {
        return false;
    }
    let mut reader = Reader::new(buffer);
    let mut found = false;
    for _ in 0..report_count {
        let Some(block) = parse_single_report_block(&mut reader) else {
            return false;
        };
        if block.ssrc == target_ssrc {
            *out = block;
            found = true;
        }
    }
    found
}

fn parse_sender_report_payload(
    mut chunk_reader: Reader<'_>,
    report_count: usize,
    sender_ssrc: u32,
    receiver_ssrc: u32,
) -> Option<ffi::WireSenderReport> {
    if chunk_reader.read_u32_be()? != sender_ssrc {
        return None;
    }
    let ntp_timestamp = chunk_reader.read_u64_be()?;
    let truncated_rtp_timestamp = chunk_reader.read_u32_be()?;
    let send_packet_count = chunk_reader.read_u32_be()?;
    let send_octet_count = chunk_reader.read_u32_be()?;

    let mut report_block = ffi::WireRtcpReportBlock::default();
    let has_report_block = parse_rtcp_report_block(
        chunk_reader.read_slice(chunk_reader.remaining())?,
        report_count,
        receiver_ssrc,
        &mut report_block,
    );

    Some(ffi::WireSenderReport {
        has_sender_report: true,
        ntp_timestamp,
        truncated_rtp_timestamp,
        send_packet_count,
        send_octet_count,
        has_report_block,
        report_block,
    })
}

pub fn parse_sender_report_packet(
    buffer: &[u8],
    sender_ssrc: u32,
    receiver_ssrc: u32,
    out: &mut ffi::WireSenderReport,
) -> bool {
    *out = ffi::WireSenderReport::default();
    let mut reader = Reader::new(buffer);
    while !reader.is_empty() {
        let Some(header) = parse_common_header(&mut reader) else {
            return false;
        };
        let Some(chunk_reader) = reader.sub_reader(header.payload_size) else {
            return false;
        };
        if header.packet_type == RtcpPacketType::SenderReport as u8 {
            if chunk_reader.remaining() < RTCP_SENDER_REPORT_SIZE {
                return false;
            }
            if let Some(report) = parse_sender_report_payload(
                chunk_reader,
                header.report_count_or_subtype as usize,
                sender_ssrc,
                receiver_ssrc,
            ) {
                *out = report;
            }
        }
    }
    true
}

// ============================================================================
// RTP Packet Parsing
// ============================================================================

fn parse_adaptive_latency_extension(
    reader: &mut Reader<'_>,
    num_extensions: usize,
) -> Option<(bool, u16)> {
    let mut has_new_playout_delay = false;
    let mut new_playout_delay_ms = 0u16;

    for _ in 0..num_extensions {
        let type_and_size = reader.read_u16_be()?;
        let ext_type = (type_and_size >> 10) as u8;
        let ext_size = (type_and_size & 0x03FF) as usize;
        let ext_data = reader.read_slice(ext_size)?;

        if ext_type == ADAPTIVE_LATENCY_RTP_EXTENSION_TYPE {
            let [b0, b1] = ext_data.try_into().ok()?;
            has_new_playout_delay = true;
            new_playout_delay_ms = u16::from_be_bytes([b0, b1]);
        }
    }
    Some((has_new_playout_delay, new_playout_delay_ms))
}

fn parse_rtp_packet_internal(buffer: &[u8], sender_ssrc: u32) -> Option<ffi::WireRtpPacket> {
    let mut reader = Reader::new(buffer);
    if reader.read_u8()? != RTP_REQUIRED_FIRST_BYTE {
        return None;
    }
    let payload_type = reader.read_u8()? & RTP_PAYLOAD_TYPE_MASK;
    if !is_rtp_payload_type(payload_type) {
        return None;
    }
    let sequence_number = reader.read_u16_be()?;
    let truncated_rtp_timestamp = reader.read_u32_be()?;
    if reader.read_u32_be()? != sender_ssrc {
        return None;
    }

    let byte12 = reader.read_u8()?;
    let is_key_frame = (byte12 & RTP_KEY_FRAME_BIT_MASK) != 0;
    let has_referenced_frame_id = (byte12 & RTP_HAS_REF_FRAME_ID_BIT_MASK) != 0;
    let num_cast_extensions = (byte12 & RTP_EXTENSION_COUNT_MASK) as usize;

    let truncated_frame_id = reader.read_u8()?;
    let packet_id = reader.read_u16_be()?;
    let max_packet_id = reader.read_u16_be()?;
    if max_packet_id == ALL_PACKETS_LOST || packet_id > max_packet_id {
        return None;
    }

    let mut truncated_referenced_frame_id = 0u8;
    if has_referenced_frame_id {
        truncated_referenced_frame_id = reader.read_u8()?;
    }

    let (has_new_playout_delay, new_playout_delay_ms) =
        parse_adaptive_latency_extension(&mut reader, num_cast_extensions)?;

    let payload_offset = reader.offset();
    let payload_len = reader.remaining();

    Some(ffi::WireRtpPacket {
        payload_type,
        sequence_number,
        truncated_rtp_timestamp,
        is_key_frame,
        truncated_frame_id,
        packet_id,
        max_packet_id,
        has_referenced_frame_id,
        truncated_referenced_frame_id,
        has_new_playout_delay,
        new_playout_delay_ms,
        payload_offset,
        payload_len,
    })
}

pub fn parse_rtp_packet(buffer: &[u8], sender_ssrc: u32, out: &mut ffi::WireRtpPacket) -> bool {
    if let Some(packet) = parse_rtp_packet_internal(buffer, sender_ssrc) {
        *out = packet;
        true
    } else {
        false
    }
}

// ============================================================================
// Compound RTCP & Feedback Parsing
// ============================================================================

fn parse_frame_log_messages(
    slice: &[u8],
    messages: &mut Vec<ffi::WireReceiverFrameLogMessage>,
) -> bool {
    let mut reader = Reader::new(slice);
    while !reader.is_empty() {
        let Some(truncated_rtp_timestamp) = reader.read_u32_be() else {
            messages.clear();
            return false;
        };
        let Some(data) = reader.read_u32_be() else {
            messages.clear();
            return false;
        };

        let raw_timestamp_ms = data & 0x00FF_FFFF;
        let num_events = 1usize + ((data >> 24) as usize);
        let mut msg = ffi::WireReceiverFrameLogMessage {
            truncated_rtp_timestamp,
            raw_timestamp_ms,
            events: Vec::with_capacity(num_events),
        };

        for _ in 0..num_events {
            let Some(delay_delta_or_packet_id) = reader.read_u16_be() else {
                messages.clear();
                return false;
            };
            let Some(event_type_and_timestamp_delta) = reader.read_u16_be() else {
                messages.clear();
                return false;
            };

            let wire_type = (event_type_and_timestamp_delta >> 12) as u8;
            let timestamp_delta_ms = event_type_and_timestamp_delta & 0x0FFF;
            msg.events.push(ffi::WireReceiverEventLog {
                wire_type,
                timestamp_delta_ms,
                delay_delta_or_packet_id,
            });
        }
        messages.push(msg);
    }
    true
}

fn canonicalize_packet_nacks(nacks: &mut Vec<ffi::WirePacketNack>) {
    nacks.sort_by_key(|n| (n.frame_id, n.packet_id.wrapping_add(1)));
    nacks.dedup_by(|b, a| {
        a.frame_id == b.frame_id && (a.packet_id == ALL_PACKETS_LOST || a.packet_id == b.packet_id)
    });
}

fn handle_receiver_report(
    mut payload_reader: Reader<'_>,
    report_count: usize,
    receiver_ssrc: u32,
    sender_ssrc: u32,
    out: &mut ffi::WireCompoundRtcp,
) -> bool {
    if payload_reader.remaining() < RTCP_RECEIVER_REPORT_SIZE {
        return false;
    }
    let Some(ssrc) = payload_reader.read_u32_be() else {
        return false;
    };
    if ssrc == receiver_ssrc {
        let mut rb = ffi::WireRtcpReportBlock::default();
        let Some(slice) = payload_reader.read_slice(payload_reader.remaining()) else {
            return false;
        };
        out.has_receiver_report =
            parse_rtcp_report_block(slice, report_count, sender_ssrc, &mut rb);
        out.receiver_report = rb;
    }
    true
}

fn handle_app_defined(
    mut payload_reader: Reader<'_>,
    subtype: u8,
    receiver_ssrc: u32,
    out: &mut ffi::WireCompoundRtcp,
) -> bool {
    if payload_reader.remaining() < 8 {
        return false;
    }
    let Some(app_ssrc) = payload_reader.read_u32_be() else {
        return false;
    };
    let Some(name) = payload_reader.read_u32_be() else {
        return false;
    };
    if app_ssrc != receiver_ssrc {
        return true;
    }
    if name != CAST_IDENTIFIER_WORD {
        return name == TIME_SYNC_REQUEST_NAME;
    }
    if subtype == RtcpSubtype::ReceiverLog as u8 {
        let Some(log_slice) = payload_reader.read_slice(payload_reader.remaining()) else {
            return false;
        };
        return parse_frame_log_messages(log_slice, &mut out.log_messages);
    }
    true
}

fn handle_picture_loss_indicator(
    mut payload_reader: Reader<'_>,
    receiver_ssrc: u32,
    sender_ssrc: u32,
    out: &mut ffi::WireCompoundRtcp,
) -> bool {
    if payload_reader.remaining() < RTCP_PICTURE_LOSS_INDICATOR_HEADER_SIZE {
        return false;
    }
    let Some(r_ssrc) = payload_reader.read_u32_be() else {
        return false;
    };
    let Some(s_ssrc) = payload_reader.read_u32_be() else {
        return false;
    };
    if r_ssrc == receiver_ssrc && s_ssrc == sender_ssrc {
        out.picture_loss_indicator = true;
    }
    true
}

fn parse_feedback_loss_nacks(
    reader: &mut Reader<'_>,
    feedback_frame_id: i64,
    count: usize,
    packet_nacks: &mut Vec<ffi::WirePacketNack>,
) -> bool {
    let Some(loss_bytes) = count.checked_mul(RTCP_FEEDBACK_LOSS_FIELD_SIZE) else {
        return false;
    };
    if reader.remaining() < loss_bytes {
        return false;
    }
    for _ in 0..count {
        let Some(truncated_fid) = reader.read_u8() else {
            return false;
        };
        let Some(mut packet_id) = reader.read_u16_be() else {
            return false;
        };
        let Some(mut bits) = reader.read_u8() else {
            return false;
        };
        let frame_id = expand_greater_than_u8(feedback_frame_id, truncated_fid);
        packet_nacks.push(ffi::WirePacketNack { frame_id, packet_id });
        if packet_id != ALL_PACKETS_LOST {
            while bits != 0 {
                packet_id = packet_id.wrapping_add(1);
                if (bits & 1) != 0 {
                    packet_nacks.push(ffi::WirePacketNack { frame_id, packet_id });
                }
                bits >>= 1;
            }
        }
    }
    true
}

fn parse_feedback_ack_bitvectors(
    reader: &mut Reader<'_>,
    feedback_frame_id: i64,
    received_frames: &mut Vec<i64>,
) -> bool {
    if reader.remaining() < RTCP_FEEDBACK_ACK_HEADER_SIZE {
        return true;
    }
    if let Some(magic) = reader.read_u32_be() {
        if magic != CST2_IDENTIFIER_WORD {
            return true;
        }
        let _ = reader.read_u8(); // unused byte
        let Some(ack_bytes_count) = reader.read_u8() else {
            return false;
        };
        let count = ack_bytes_count as usize;
        let Some(ack_slice) = reader.read_slice(count) else {
            return false;
        };
        let mut starting_frame_id = feedback_frame_id.wrapping_add(2);
        for &byte in ack_slice {
            let mut bits = byte;
            let mut frame_id = starting_frame_id;
            while bits != 0 {
                if (bits & 1) != 0 {
                    received_frames.push(frame_id);
                }
                frame_id = frame_id.wrapping_add(1);
                bits >>= 1;
            }
            starting_frame_id = starting_frame_id.wrapping_add(8);
        }
    }
    true
}

fn handle_cast_feedback(
    mut payload_reader: Reader<'_>,
    receiver_ssrc: u32,
    sender_ssrc: u32,
    max_feedback_frame_id: i64,
    out: &mut ffi::WireCompoundRtcp,
) -> bool {
    if max_feedback_frame_id == i64::MIN || payload_reader.remaining() < RTCP_FEEDBACK_HEADER_SIZE {
        return false;
    }
    let Some(r_ssrc) = payload_reader.read_u32_be() else {
        return false;
    };
    let Some(s_ssrc) = payload_reader.read_u32_be() else {
        return false;
    };
    if r_ssrc != receiver_ssrc || s_ssrc != sender_ssrc {
        return true;
    }
    if payload_reader.read_u32_be() != Some(CAST_IDENTIFIER_WORD) {
        return false;
    }
    let Some(truncated_fid) = payload_reader.read_u8() else {
        return false;
    };
    let Some(loss_field_count) = payload_reader.read_u8() else {
        return false;
    };
    let Some(playout_delay_ms) = payload_reader.read_u16_be() else {
        return false;
    };

    let feedback_frame_id = expand_less_than_or_equal_u8(max_feedback_frame_id, truncated_fid);
    if out.has_checkpoint_frame_id && out.checkpoint_frame_id > feedback_frame_id {
        return true;
    }
    out.has_checkpoint_frame_id = true;
    out.checkpoint_frame_id = feedback_frame_id;
    out.target_playout_delay_ms = playout_delay_ms;
    out.received_frames.clear();
    out.packet_nacks.clear();

    if !parse_feedback_loss_nacks(
        &mut payload_reader,
        feedback_frame_id,
        loss_field_count as usize,
        &mut out.packet_nacks,
    ) {
        return false;
    }

    parse_feedback_ack_bitvectors(&mut payload_reader, feedback_frame_id, &mut out.received_frames)
}

fn handle_extended_reports(
    mut payload_reader: Reader<'_>,
    receiver_ssrc: u32,
    out: &mut ffi::WireCompoundRtcp,
) -> bool {
    if payload_reader.remaining() < RTCP_EXTENDED_REPORT_HEADER_SIZE {
        return false;
    }
    let Some(ssrc) = payload_reader.read_u32_be() else {
        return false;
    };
    if ssrc != receiver_ssrc {
        return true;
    }
    while !payload_reader.is_empty() {
        let Some(block_type) = payload_reader.read_u8() else {
            return false;
        };
        let _ = payload_reader.read_u8(); // unused byte
        let Some(block_words) = payload_reader.read_u16_be() else {
            return false;
        };
        let block_data_size = (block_words as usize) * 4;
        let Some(mut block_reader) = payload_reader.sub_reader(block_data_size) else {
            return false;
        };
        if block_type == RTCP_RECEIVER_REF_TIME_REPORT_BLOCK_TYPE {
            if block_data_size != 8 {
                return false;
            }
            let Some(ntp_time) = block_reader.read_u64_be() else {
                return false;
            };
            out.has_receiver_reference_ntp_time = true;
            out.receiver_reference_ntp_time = ntp_time;
        }
    }
    true
}

pub fn parse_compound_rtcp(
    buffer: &[u8],
    receiver_ssrc: u32,
    sender_ssrc: u32,
    max_feedback_frame_id: i64,
    out: &mut ffi::WireCompoundRtcp,
) -> bool {
    *out = ffi::WireCompoundRtcp::default();
    let mut reader = Reader::new(buffer);

    while !reader.is_empty() {
        let Some(header) = parse_common_header(&mut reader) else {
            return false;
        };
        let Some(payload_reader) = reader.sub_reader(header.payload_size) else {
            return false;
        };

        let ok = match RtcpPacketType::from_repr(header.packet_type) {
            Some(RtcpPacketType::ReceiverReport) => handle_receiver_report(
                payload_reader,
                header.report_count_or_subtype as usize,
                receiver_ssrc,
                sender_ssrc,
                out,
            ),
            Some(RtcpPacketType::ApplicationDefined) => handle_app_defined(
                payload_reader,
                header.report_count_or_subtype,
                receiver_ssrc,
                out,
            ),
            Some(RtcpPacketType::PayloadSpecific) => {
                match RtcpSubtype::from_repr(header.report_count_or_subtype) {
                    Some(RtcpSubtype::PictureLossIndicator) => handle_picture_loss_indicator(
                        payload_reader,
                        receiver_ssrc,
                        sender_ssrc,
                        out,
                    ),
                    Some(RtcpSubtype::Feedback) => handle_cast_feedback(
                        payload_reader,
                        receiver_ssrc,
                        sender_ssrc,
                        max_feedback_frame_id,
                        out,
                    ),
                    _ => true,
                }
            }
            Some(RtcpPacketType::ExtendedReports) => {
                handle_extended_reports(payload_reader, receiver_ssrc, out)
            }
            _ => true,
        };

        if !ok {
            return false;
        }
    }

    canonicalize_packet_nacks(&mut out.packet_nacks);
    true
}
