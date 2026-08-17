// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/std_util.h"

#include <array>
#include <set>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "util/string_util.h"

namespace openscreen {

TEST(StdUtilTest, Data) {
  std::string non_empty("Where no one has gone before");
  EXPECT_TRUE(data(non_empty) != nullptr);
  EXPECT_EQ(data(non_empty), non_empty.data());

  std::string empty;
  EXPECT_TRUE(data(empty) != nullptr);
  EXPECT_EQ(data(empty), empty.data());
}

TEST(StdUtilTest, RemoveWhitespace) {
  std::string portland = "Portland";
  EXPECT_EQ("Portland", RemoveWhitespace(portland));

  std::string fancy_portland = "  Po\f\v\tr\t\ntla  n\r\nd\t\t  ";
  RemoveWhitespace(fancy_portland);
  EXPECT_EQ("Portland", fancy_portland);
}

TEST(StdUtilTest, AreElementSortedAndUnique) {
  EXPECT_TRUE(AreElementsSortedAndUnique(std::vector<std::string>({})));
  EXPECT_TRUE(AreElementsSortedAndUnique(std::vector<std::string>({"Joey"})));
  EXPECT_TRUE(AreElementsSortedAndUnique(
      std::vector<std::string>({"Chandler", "Joey", "Phoebe"})));
  EXPECT_FALSE(AreElementsSortedAndUnique(
      std::vector<std::string>({"Chandler", "Joey", "Joey", "Phoebe"})));
  EXPECT_FALSE(AreElementsSortedAndUnique(
      std::vector<std::string>({"Chandler", "Phoebe", "Joey"})));
}

TEST(StdUtilTest, SortAndDedupeElements) {
  auto empty_vector = std::vector<std::string>({});
  SortAndDedupeElements(empty_vector);
  EXPECT_TRUE(empty_vector.empty());

  auto singleton_vector = std::vector<std::string>({"Joey"});
  SortAndDedupeElements(singleton_vector);
  EXPECT_EQ(singleton_vector, std::vector<std::string>({"Joey"}));

  auto all_friends =
      std::vector<std::string>({"Joey", "Rachel", "Monica", "Chandler",
                                "Phoebe", "Ross", "Rachel", "Joey"});
  SortAndDedupeElements(all_friends);
  EXPECT_EQ(all_friends,
            std::vector<std::string>(
                {"Chandler", "Joey", "Monica", "Phoebe", "Rachel", "Ross"}));

  std::vector<int> nums = {5, 2, 8, 2, 5, 1};
  SortAndDedupeElements(nums);
  EXPECT_EQ(nums, (std::vector<int>{1, 2, 5, 8}));
}

TEST(StdUtilTest, Append) {
  std::vector<std::string> one_friend({"Joey"});
  auto friends = Append(std::move(one_friend), "Rachel", "Monica", "Chandler",
                        "Phoebe", "Ross");
  EXPECT_EQ(std::vector<std::string>(
                {"Joey", "Rachel", "Monica", "Chandler", "Phoebe", "Ross"}),
            friends);
}

TEST(StdUtilTest, GetVectorWithCapacity) {
  auto ten_strings(GetVectorWithCapacity<std::string>(10));
  EXPECT_EQ(static_cast<size_t>(0), ten_strings.size());
  EXPECT_EQ(static_cast<size_t>(10), ten_strings.capacity());
}

TEST(StdUtilTest, Contains) {
  auto friends = std::vector<std::string>(
      {"Joey", "Rachel", "Monica", "Chandler", "Phoebe", "Ross"});
  EXPECT_TRUE(Contains(friends, "Rachel"));
  EXPECT_FALSE(Contains(friends, "Ursula"));

  // Associative container with .contains()
  std::set<std::string> friend_set = {"Joey", "Monica", "Chandler"};
  EXPECT_TRUE(Contains(friend_set, "Joey"));
  EXPECT_FALSE(Contains(friend_set, "Ross"));
}

TEST(StdUtilTest, ContainsIf) {
  auto friends = std::vector<std::string>(
      {"Joey", "Rachel", "Monica", "Chandler", "Phoebe", "Ross"});
  EXPECT_TRUE(
      ContainsIf(friends, [](const auto& f) { return f == "Chandler"; }));
  EXPECT_FALSE(
      ContainsIf(friends, [](const auto& f) { return f == "Ursula"; }));
}

static_assert(AreElementsSortedAndUnique(std::array<int, 4>{1, 3, 5, 7}));
static_assert(!AreElementsSortedAndUnique(std::array<int, 4>{1, 3, 3, 7}));
static_assert(!AreElementsSortedAndUnique(std::array<int, 4>{1, 5, 3, 7}));

}  // namespace openscreen
