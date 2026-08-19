// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/json/json_serialization.h"

#include <array>
#include <string>

#include "gtest/gtest.h"
#include "platform/base/error.h"

namespace openscreen {
namespace {
template <typename Value>
void AssertError(ErrorOr<Value> error_or, Error::Code code) {
  EXPECT_EQ(error_or.error().code(), code);
}
}  // namespace

TEST(JsonSerializationTest, MalformedDocumentReturnsParseError) {
  const std::array<std::string, 4> kMalformedDocuments{
      {"", "{", "{ foo: bar }", R"({"foo": "bar", "foo": baz})"}};

  for (auto& document : kMalformedDocuments) {
    AssertError(json::Parse(document), Error::Code::kJsonParseError);
  }
}

TEST(JsonSerializationTest, ValidEmptyDocumentParsedCorrectly) {
  const auto actual = json::Parse("{}");

  EXPECT_TRUE(actual.is_value());
  EXPECT_EQ(actual.value().getMemberNames().size(), 0u);
}

TEST(JsonSerializationTest, ValidDocumentParsedCorrectly) {
  const auto actual = json::Parse(R"({"foo": "bar", "baz": 1337})");

  EXPECT_TRUE(actual.is_value());
  EXPECT_EQ(actual.value().getMemberNames().size(), 2u);
  EXPECT_EQ(actual.value()["foo"].asString(), "bar");
  EXPECT_EQ(actual.value()["baz"].asInt(), 1337);
}

TEST(JsonSerializationTest, ComplexNestedDocumentParsedCorrectly) {
  const auto actual = json::Parse(
      R"({
        "string": "hello world",
        "int": 42,
        "bool": true,
        "null_val": null,
        "array": [1, "two", false],
        "nested": {"key": "val"}
      })");

  EXPECT_TRUE(actual.is_value());
  const auto& val = actual.value();
  EXPECT_EQ(val["string"].asString(), "hello world");
  EXPECT_EQ(val["int"].asInt(), 42);
  EXPECT_EQ(val["bool"].asBool(), true);
  EXPECT_TRUE(val["null_val"].isNull());
  EXPECT_EQ(val["array"].size(), 3u);
  EXPECT_EQ(val["array"][0].asInt(), 1);
  EXPECT_EQ(val["array"][1].asString(), "two");
  EXPECT_EQ(val["array"][2].asBool(), false);
  EXPECT_EQ(val["nested"]["key"].asString(), "val");
}

TEST(JsonSerializationTest, EmptyArrayReturnsBrackets) {
  const auto empty_array = Json::Value(Json::ValueType::arrayValue);
  const auto actual = json::Stringify(empty_array);

  EXPECT_TRUE(actual.is_value());
  EXPECT_EQ(actual.value(), "[]");
}

TEST(JsonSerializationTest, NullValueReturnsNull) {
  const auto null_value = Json::Value();
  const auto actual = json::Stringify(null_value);

  EXPECT_TRUE(actual.is_value());
  EXPECT_EQ(actual.value(), "null");
}

TEST(JsonSerializationTest, ValidValueReturnsString) {
  const Json::Int64 value = 31337;
  const auto actual = json::Stringify(value);

  EXPECT_TRUE(actual.is_value());
  EXPECT_EQ(actual.value(), "31337");
}

TEST(JsonSerializationTest, BooleanValuesStringified) {
  EXPECT_EQ(json::Stringify(Json::Value(true)).value(), "true");
  EXPECT_EQ(json::Stringify(Json::Value(false)).value(), "false");
}

TEST(JsonSerializationTest, ArrayAndObjectRoundTrip) {
  const auto parsed = json::Parse(R"({"numbers":[1,2,3],"tag":"test"})");
  ASSERT_TRUE(parsed.is_value());
  const auto serialized = json::Stringify(parsed.value());
  ASSERT_TRUE(serialized.is_value());

  const auto reparsed = json::Parse(serialized.value());
  ASSERT_TRUE(reparsed.is_value());
  EXPECT_EQ(reparsed.value()["tag"].asString(), "test");
  EXPECT_EQ(reparsed.value()["numbers"].size(), 3u);
  EXPECT_EQ(reparsed.value()["numbers"][0].asInt(), 1);
  EXPECT_EQ(reparsed.value()["numbers"][1].asInt(), 2);
  EXPECT_EQ(reparsed.value()["numbers"][2].asInt(), 3);
}

}  // namespace openscreen
