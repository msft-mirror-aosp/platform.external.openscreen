// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/impl/quic/quic_utils.h"

#include <stdint.h>

#include <span>
#include <utility>

#include "quiche/common/quiche_ip_address.h"
#include "util/osp_logging.h"

namespace openscreen {

quiche::QuicheIpAddress ToQuicheIpAddress(const IPAddress& address) {
  if (address.IsV4()) {
    in_addr result{};
    static_assert(sizeof(result) == IPAddress::kV4Size,
                  "Address size mismatch");
    address.CopyTo(std::span<uint8_t>(
        reinterpret_cast<uint8_t*>(&result.s_addr), IPAddress::kV4Size));
    return quiche::QuicheIpAddress(result);
  }

  if (address.IsV6()) {
    in6_addr result;
    static_assert(sizeof(result) == IPAddress::kV6Size,
                  "Address size mismatch");
    address.CopyTo(std::span<uint8_t>(result.s6_addr, 16));
    return quiche::QuicheIpAddress(result);
  }

  return quiche::QuicheIpAddress();
}

quic::QuicSocketAddress ToQuicSocketAddress(const IPEndpoint& endpoint) {
  return quic::QuicSocketAddress(ToQuicheIpAddress(endpoint.address),
                                 endpoint.port);
}

IPEndpoint ToIPEndpoint(const quic::QuicSocketAddress& address) {
  if (!address.IsInitialized()) {
    return IPEndpoint{};
  }

  const quiche::QuicheIpAddress ip = address.host();
  const uint16_t port = address.port();

  if (ip.IsIPv4()) {
    const in_addr ipv4 = ip.GetIPv4();
    IPAddress ip_addr(
        IPAddress::Version::kV4,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&ipv4.s_addr),
                                 IPAddress::kV4Size));
    return IPEndpoint{std::move(ip_addr), port};
  }

  if (ip.IsIPv6()) {
    const in6_addr ipv6 = ip.GetIPv6();
    IPAddress ip_addr(IPAddress::Version::kV6,
                      std::span<const uint8_t>(
                          reinterpret_cast<const uint8_t*>(&ipv6.s6_addr),
                          IPAddress::kV6Size));
    return IPEndpoint{std::move(ip_addr), port};
  }

  return IPEndpoint{};
}

}  // namespace openscreen
