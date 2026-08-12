// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UTIL_STRING_PARSE_H_
#define UTIL_STRING_PARSE_H_

#include <charconv>
#include <concepts>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace openscreen {

// Parses `number` into the integer type `result` and returns true if
// successful and the entire string was consumed. `number` must be an ASCII
// representation of an integer. If `number` cannot be parsed or contains
// trailing invalid characters, returns false.
template <typename T>
  requires(std::integral<T>)
bool ParseAsciiNumber(std::string_view number, T& result, int base = 10) {
  if (number.empty()) {
    return false;
  }
  auto [ptr, error_code] = std::from_chars(
      number.data(), number.data() + number.size(), result, base);
  return error_code == std::errc() && ptr == number.data() + number.size();
}

// Parses `number` into the floating-point type `result` and returns true if
// successful and the entire string was consumed.
template <typename T>
  requires(std::floating_point<T>)
bool ParseAsciiNumber(std::string_view number,
                      T& result,
                      std::chars_format fmt = std::chars_format::general) {
  if (number.empty()) {
    return false;
  }
  auto [ptr, error_code] = std::from_chars(
      number.data(), number.data() + number.size(), result, fmt);
  return error_code == std::errc() && ptr == number.data() + number.size();
}

// Parses `number` into `std::optional<T>`. Returns std::nullopt on failure.
template <typename T, typename... Args>
  requires(std::integral<T> || std::floating_point<T>)
std::optional<T> ParseAsciiNumber(std::string_view number, Args&&... args) {
  T result{};
  if (ParseAsciiNumber(number, result, std::forward<Args>(args)...)) {
    return result;
  }
  return std::nullopt;
}

}  // namespace openscreen

#endif  // UTIL_STRING_PARSE_H_
