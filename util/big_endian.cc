// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/big_endian.h"

#include <ranges>

namespace openscreen {

BigEndianReader::BigEndianReader(ByteView buffer) : BigEndianBuffer(buffer) {}

BigEndianReader::BigEndianReader(const uint8_t* buffer, size_t length)
    : BigEndianBuffer(buffer, length) {}

bool BigEndianReader::Read(size_t length, void* out) {
  return Read(ByteBuffer(static_cast<uint8_t*>(out), length));
}

bool BigEndianReader::Read(ByteBuffer out) {
  ByteView view = remaining_span();
  if (view.size() >= out.size()) {
    std::ranges::copy(view.first(out.size()), out.begin());
    Skip(out.size());
    return true;
  }
  return false;
}

BigEndianWriter::BigEndianWriter(ByteBuffer buffer) : BigEndianBuffer(buffer) {}

bool BigEndianWriter::Write(ByteView buffer) {
  ByteBuffer view = remaining_span();
  if (view.size() >= buffer.size()) {
    std::ranges::copy(buffer, view.begin());
    Skip(buffer.size());
    return true;
  }
  return false;
}

}  // namespace openscreen
