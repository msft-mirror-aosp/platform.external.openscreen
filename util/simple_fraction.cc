// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/simple_fraction.h"

#include <cmath>
#include <format>
#include <limits>
#include <string>
#include <vector>

#include "util/osp_logging.h"
#include "util/string_parse.h"
#include "util/string_util.h"

namespace openscreen {

// static
ErrorOr<SimpleFraction> SimpleFraction::FromString(std::string_view value) {
  if (value.size() > 0 && value.at(0) == '/') {
    return Error::Code::kParameterInvalid;
  }

  std::vector<std::string_view> fields = Split(value, '/');
  if (fields.size() != 1 && fields.size() != 2) {
    return Error::Code::kParameterInvalid;
  }

  int numerator;
  int denominator = 1;
  if (!ParseAsciiNumber(fields[0], numerator)) {
    return Error::Code::kParameterInvalid;
  }

  if (fields.size() == 2) {
    if (!ParseAsciiNumber(fields[1], denominator)) {
      return Error::Code::kParameterInvalid;
    }
  }

  return SimpleFraction(numerator, denominator);
}

std::string SimpleFraction::ToString() const {
  if (denominator_ == 1) {
    return std::to_string(numerator_);
  }
  return std::format("{}/{}", numerator_, denominator_);
}

}  // namespace openscreen
