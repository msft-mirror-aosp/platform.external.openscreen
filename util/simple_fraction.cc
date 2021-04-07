// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/simple_fraction.h"

#include <cmath>
#include <limits>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "util/osp_logging.h"

namespace openscreen {

// static
ErrorOr<SimpleFraction> SimpleFraction::FromString(absl::string_view value) {
  std::vector<absl::string_view> fields = absl::StrSplit(value, '/');
  if (fields.size() != 1 && fields.size() != 2) {
    return Error::Code::kParameterInvalid;
  }

  int numerator;
  int denominator = 1;
  if (!absl::SimpleAtoi(fields[0], &numerator)) {
    return Error::Code::kParameterInvalid;
  }

  if (fields.size() == 2) {
    if (!absl::SimpleAtoi(fields[1], &denominator)) {
      return Error::Code::kParameterInvalid;
    }
  }

  return SimpleFraction(numerator, denominator);
}

std::string SimpleFraction::ToString() const {
  if (denominator_ == 1) {
    return std::to_string(numerator_);
  }
  return absl::StrCat(numerator_, "/", denominator_);
}

SimpleFraction::SimpleFraction() : numerator_(0), denominator_(1) {}

SimpleFraction::SimpleFraction(int numerator, int denominator)
    : numerator_(numerator), denominator_(denominator) {}

SimpleFraction::SimpleFraction(int numerator)
    : numerator_(numerator), denominator_(1) {}

SimpleFraction::SimpleFraction(const SimpleFraction&) = default;
SimpleFraction::SimpleFraction(SimpleFraction&&) = default;
SimpleFraction& SimpleFraction::operator=(const SimpleFraction&) = default;
SimpleFraction& SimpleFraction::operator=(SimpleFraction&&) = default;
SimpleFraction::~SimpleFraction() = default;

bool SimpleFraction::operator==(const SimpleFraction& other) const {
  return numerator_ == other.numerator_ && denominator_ == other.denominator_;
}

bool SimpleFraction::operator!=(const SimpleFraction& other) const {
  return !(*this == other);
}

bool SimpleFraction::is_defined() const {
  return denominator_ != 0;
}

bool SimpleFraction::is_positive() const {
  return is_defined() && (numerator_ >= 0) && (denominator_ > 0);
}

SimpleFraction::operator double() const {
  if (denominator_ == 0) {
    return nan("");
  }
  return static_cast<double>(numerator_) / static_cast<double>(denominator_);
}

}  // namespace openscreen
