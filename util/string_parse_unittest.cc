// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/string_parse.h"

#include <limits>
#include <string_view>

#include "gtest/gtest.h"

namespace openscreen {

void ExpectParseInt(std::string_view number, int expected_value) {
  int result = 0;
  EXPECT_TRUE(ParseAsciiNumber(number, result));
  EXPECT_EQ(expected_value, result);
}

TEST(StringParseTest, ParseAsciiNumberInt) {
  ExpectParseInt("0", 0);
  ExpectParseInt("0100", 100);
  ExpectParseInt("13245", 13245);
  ExpectParseInt("-77377", -77377);
  ExpectParseInt("-2147483648", std::numeric_limits<int>::min());
  ExpectParseInt("2147483647", std::numeric_limits<int>::max());
}

TEST(StringParseTest, ParseAsciiNumberFails) {
  int result = 0;
  EXPECT_FALSE(ParseAsciiNumber("", result));
  EXPECT_FALSE(ParseAsciiNumber("- 100", result));
  EXPECT_FALSE(ParseAsciiNumber("ASXD", result));
  EXPECT_FALSE(ParseAsciiNumber("  100", result));
  EXPECT_FALSE(ParseAsciiNumber("-2147483649", result));
  EXPECT_FALSE(ParseAsciiNumber("2147483648", result));
  // Rejects trailing garbage:
  EXPECT_FALSE(ParseAsciiNumber("123abc", result));
  EXPECT_FALSE(ParseAsciiNumber("100 ", result));
  EXPECT_FALSE(ParseAsciiNumber("42px", result));
}

TEST(StringParseTest, ParseAsciiNumberHexAndBinary) {
  int result = 0;
  EXPECT_TRUE(ParseAsciiNumber("ff", result, 16));
  EXPECT_EQ(255, result);

  EXPECT_TRUE(ParseAsciiNumber("1010", result, 2));
  EXPECT_EQ(10, result);
}

TEST(StringParseTest, ParseAsciiNumberFloatingPoint) {
  double d = 0.0;
  EXPECT_TRUE(ParseAsciiNumber("3.14159", d));
  EXPECT_NEAR(3.14159, d, 1e-5);

  EXPECT_TRUE(ParseAsciiNumber("-0.001", d));
  EXPECT_NEAR(-0.001, d, 1e-5);

  EXPECT_FALSE(ParseAsciiNumber("3.14abc", d));
}

TEST(StringParseTest, ParseAsciiNumberOptional) {
  EXPECT_EQ(42, ParseAsciiNumber<int>("42"));
  EXPECT_EQ(-10, ParseAsciiNumber<int>("-10"));
  EXPECT_EQ(std::nullopt, ParseAsciiNumber<int>("invalid"));
  EXPECT_EQ(std::nullopt, ParseAsciiNumber<int>("42trailing"));

  const auto maybe_double = ParseAsciiNumber<double>("2.718");
  ASSERT_TRUE(maybe_double.has_value());
  EXPECT_NEAR(2.718, *maybe_double, 1e-3);
}

}  // namespace openscreen
