// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "discovery/common/config.h"

#if defined(USE_RUST_MDNS_PARSER)
#include "discovery/mdns/public/simple_mdns_reader.h"
#else
#include "discovery/mdns/public/mdns_reader.h"
#endif

namespace openscreen::discovery {

void Fuzz(const uint8_t* data, size_t size) {
#if defined(USE_RUST_MDNS_PARSER)
  SimpleMdnsReader::Read(Config{}, ByteView(data, size));
#else
  MdnsReader reader(Config{}, data, size);
  reader.Read();
#endif
}

}  // namespace openscreen::discovery

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  openscreen::discovery::Fuzz(data, size);
  return 0;
}
