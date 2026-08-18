// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/streaming/message_fields.h"

#include <array>
#include <cstring>
#include <vector>

#include "gtest/gtest.h"

namespace openscreen::cast {
namespace {

// NOTE: We don't do an exhaustive check of all values here, to avoid
// unnecessary duplication, but want to ensure that lookup is working properly.
TEST(MessageFieldsTest, CanParseEnumToString) {
  EXPECT_STREQ("aac", CodecToString(AudioCodec::kAac));
  EXPECT_STREQ("vp8", CodecToString(VideoCodec::kVp8));
}

TEST(MessageFieldsTest, CanStringToEnum) {
  EXPECT_EQ(AudioCodec::kOpus, StringToAudioCodec("opus").value());
  EXPECT_EQ(VideoCodec::kHevc, StringToVideoCodec("hevc").value());
  EXPECT_EQ(VideoCodec::kH264, StringToVideoCodec("h264").value());
  EXPECT_FALSE(StringToVideoCodec("h265").is_value());
}

TEST(MessageFieldsTest, Identity) {
  EXPECT_STREQ("opus", CodecToString(StringToAudioCodec("opus").value()));
  EXPECT_STREQ("vp8", CodecToString(StringToVideoCodec("vp8").value()));
  EXPECT_STREQ("h264", CodecToString(StringToVideoCodec("h264").value()));
  EXPECT_STREQ("hevc", CodecToString(StringToVideoCodec("hevc").value()));
}

}  // namespace
}  // namespace openscreen::cast
