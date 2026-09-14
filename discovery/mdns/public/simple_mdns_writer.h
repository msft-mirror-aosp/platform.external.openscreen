// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DISCOVERY_MDNS_PUBLIC_SIMPLE_MDNS_WRITER_H_
#define DISCOVERY_MDNS_PUBLIC_SIMPLE_MDNS_WRITER_H_

#include <cstdint>
#include <vector>

#include "discovery/mdns/public/mdns_records.h"
#include "platform/base/error.h"

namespace openscreen::discovery {

class SimpleMdnsWriter {
 public:
  SimpleMdnsWriter() = delete;

  // Serializes an MdnsMessage into compressed mDNS wire format using
  // simple-dns. Returns a vector of serialized bytes, or
  // Error::Code::kInsufficientBuffer on failure.
  static ErrorOr<std::vector<uint8_t>> Write(const MdnsMessage& message);
};

}  // namespace openscreen::discovery

#endif  // DISCOVERY_MDNS_PUBLIC_SIMPLE_MDNS_WRITER_H_
