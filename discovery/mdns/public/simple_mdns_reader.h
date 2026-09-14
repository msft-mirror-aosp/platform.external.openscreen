// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DISCOVERY_MDNS_PUBLIC_SIMPLE_MDNS_READER_H_
#define DISCOVERY_MDNS_PUBLIC_SIMPLE_MDNS_READER_H_

#include <cstddef>
#include <cstdint>

#include "discovery/common/config.h"
#include "discovery/mdns/public/mdns_records.h"
#include "platform/base/error.h"
#include "platform/base/span.h"

namespace openscreen::discovery {

class SimpleMdnsReader {
 public:
  // Reads and parses a complete mDNS message from `buffer` using the
  // simple-dns wire parser.
  static ErrorOr<MdnsMessage> Read(const Config& config, ByteView buffer);

  SimpleMdnsReader(const Config& config, ByteView buffer);
  SimpleMdnsReader(const SimpleMdnsReader&) = delete;
  SimpleMdnsReader& operator=(const SimpleMdnsReader&) = delete;
  ~SimpleMdnsReader() = default;

  // Reads and parses a complete mDNS message from the buffer provided
  // to the constructor.
  ErrorOr<MdnsMessage> Read() const;

 private:
  const Config config_;
  ByteView buffer_;
};

}  // namespace openscreen::discovery

#endif  // DISCOVERY_MDNS_PUBLIC_SIMPLE_MDNS_READER_H_
