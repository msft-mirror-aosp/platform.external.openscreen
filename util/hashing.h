// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UTIL_HASHING_H_
#define UTIL_HASHING_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

namespace openscreen {

// This value is taken from absl::Hash implementation.
inline constexpr size_t kDefaultSeed =
    static_cast<size_t>(UINT64_C(0xc3a5c85c97cb3127));

// Computes the aggregate hash of the provided hashable objects.
// Seed must initially use a large prime as a starting value, or the result of a
// previous call to this function.
template <typename... T>
constexpr size_t ComputeAggregateHash(size_t original_seed, const T&... objs) {
  constexpr auto hash_combiner = [](size_t current_seed,
                                    size_t hash_value) -> size_t {
    constexpr uint64_t kMultiplier = UINT64_C(0x9ddfea08eb382d69);
    uint64_t a = (static_cast<uint64_t>(hash_value) ^
                  static_cast<uint64_t>(current_seed)) *
                 kMultiplier;
    a ^= (a >> 47);
    uint64_t b = (static_cast<uint64_t>(current_seed) ^ a) * kMultiplier;
    b ^= (b >> 47);
    b *= kMultiplier;
    return static_cast<size_t>(b);
  };

  size_t result = original_seed;
  ((result = hash_combiner(result, std::hash<T>{}(objs))), ...);
  return result;
}

template <typename... T>
constexpr size_t ComputeAggregateHash(const T&... objs) {
  return ComputeAggregateHash(kDefaultSeed, objs...);
}

struct PairHash {
  template <typename TFirst, typename TSecond>
  size_t operator()(const std::pair<TFirst, TSecond>& pair) const {
    return ComputeAggregateHash(pair.first, pair.second);
  }
};

}  // namespace openscreen

#endif  // UTIL_HASHING_H_
