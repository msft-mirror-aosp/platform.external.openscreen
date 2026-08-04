// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PLATFORM_IMPL_QUIC_QUIC_PACKET_WRITER_IMPL_H_
#define PLATFORM_IMPL_QUIC_QUIC_PACKET_WRITER_IMPL_H_

#include "platform/api/udp_socket.h"
#include "quiche/quic/core/quic_packet_writer.h"
#include "util/raw_ptr.h"

namespace openscreen {

class PacketWriterImpl final : public quic::QuicPacketWriter {
 public:
  explicit PacketWriterImpl(UdpSocket* socket);
  PacketWriterImpl(const PacketWriterImpl&) = delete;
  PacketWriterImpl& operator=(const PacketWriterImpl&) = delete;
  PacketWriterImpl(PacketWriterImpl&&) noexcept = delete;
  PacketWriterImpl& operator=(PacketWriterImpl&&) noexcept = delete;
  ~PacketWriterImpl() override;

  // quic::QuicPacketWriter overrides.
  quic::WriteResult WritePacket(
      const char* buffer,
      size_t buffer_length,
      const quiche::QuicheIpAddress& self_address,
      const quic::QuicSocketAddress& peer_address,
      quic::PerPacketOptions* options,
      const quic::QuicPacketWriterParams& params) override;
  bool IsWriteBlocked() const override;
  void SetWritable() override;
  std::optional<int> MessageTooBigErrorCode() const override;
  quic::QuicByteCount GetMaxPacketSize(
      const quic::QuicSocketAddress& peer_address) const override;
  bool SupportsReleaseTime() const override;
  bool IsBatchMode() const override;
  bool SupportsEcn() const override;
  quic::QuicPacketBuffer GetNextWriteLocation(
      const quiche::QuicheIpAddress& self_address,
      const quic::QuicSocketAddress& peer_address) override;
  quic::WriteResult Flush() override;

  UdpSocket* socket() { return socket_; }

 private:
  raw_ptr<UdpSocket> socket_ = nullptr;
};

}  // namespace openscreen

#endif  // PLATFORM_IMPL_QUIC_QUIC_PACKET_WRITER_IMPL_H_
