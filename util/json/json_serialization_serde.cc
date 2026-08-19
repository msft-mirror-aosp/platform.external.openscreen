// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/json/json_serialization.h"

#if !defined(USE_JSON_SERDE)
#error "USE_JSON_SERDE not defined for json_serialization_serde.cc"
#endif

#include <utility>

#include "util/json/json_serialization_serde_internal.h"
#include "util/json/serde_json.rs.h"

namespace openscreen::json {

ErrorOr<Json::Value> Parse(std::string_view document) {
  if (document.empty()) {
    return ErrorOr<Json::Value>(Error::Code::kJsonParseError, "empty document");
  }

  serde::ValueBuilder builder;
  ::rust::String error_msg;
  ::rust::Slice<const uint8_t> slice(
      reinterpret_cast<const uint8_t*>(document.data()), document.size());

  if (!serde::decode_json_to_builder(slice, builder, error_msg)) {
    return ErrorOr<Json::Value>(Error::Code::kJsonParseError,
                                std::string(error_msg));
  }

  return builder.TakeValue();
}

ErrorOr<std::string> Stringify(const Json::Value& value) {
  serde::ValueVisitor visitor(value);
  ::rust::String output;
  ::rust::String error_msg;
#if defined(_DEBUG)
  constexpr bool kPretty = true;
#else
  constexpr bool kPretty = false;
#endif

  if (!serde::encode_json(visitor, kPretty, output, error_msg)) {
    return ErrorOr<std::string>(Error::Code::kJsonWriteError,
                                error_msg.empty() ? "JSON serialization failed"
                                                  : std::string(error_msg));
  }

  return std::string(output);
}

namespace serde {

ValueBuilder::ValueBuilder() = default;
ValueBuilder::~ValueBuilder() = default;

Json::Value* ValueBuilder::GetNextTarget() {
  if (stack_.empty()) {
    return &root_;
  }
  auto& top = stack_.back();
  if (top.type == StackEntry::Type::kArray) {
    top.container->append(Json::Value());
    return &((*top.container)[top.container->size() - 1]);
  } else {
    // kObject
    auto* target = &((*top.container)[top.pending_key]);
    top.pending_key.clear();
    return target;
  }
}

void ValueBuilder::set_null() {
  *GetNextTarget() = Json::Value(Json::nullValue);
}

void ValueBuilder::set_bool(bool val) {
  *GetNextTarget() = Json::Value(val);
}

void ValueBuilder::set_i64(int64_t val) {
  *GetNextTarget() = Json::Value(static_cast<Json::Value::Int64>(val));
}

void ValueBuilder::set_u64(uint64_t val) {
  *GetNextTarget() = Json::Value(static_cast<Json::Value::UInt64>(val));
}

void ValueBuilder::set_f64(double val) {
  *GetNextTarget() = Json::Value(val);
}

void ValueBuilder::set_string(::rust::Str val) {
  *GetNextTarget() = Json::Value(val.data(), val.data() + val.size());
}

void ValueBuilder::begin_array() {
  Json::Value* target = GetNextTarget();
  *target = Json::Value(Json::arrayValue);
  stack_.push_back(StackEntry{StackEntry::Type::kArray, target, ""});
}

void ValueBuilder::end_array() {
  if (!stack_.empty() && stack_.back().type == StackEntry::Type::kArray) {
    stack_.pop_back();
  }
}

void ValueBuilder::begin_object() {
  Json::Value* target = GetNextTarget();
  *target = Json::Value(Json::objectValue);
  stack_.push_back(StackEntry{StackEntry::Type::kObject, target, ""});
}

void ValueBuilder::set_object_key(::rust::Str key) {
  if (!stack_.empty() && stack_.back().type == StackEntry::Type::kObject) {
    stack_.back().pending_key = std::string(key.data(), key.size());
  }
}

void ValueBuilder::end_object() {
  if (!stack_.empty() && stack_.back().type == StackEntry::Type::kObject) {
    stack_.pop_back();
  }
}

Json::Value ValueBuilder::TakeValue() {
  return std::move(root_);
}

ValueVisitor::ValueVisitor(const Json::Value& root) : root_(root) {}
ValueVisitor::~ValueVisitor() = default;

bool ValueVisitor::visit_node(SerdeJsonWriter& writer) {
  return VisitValue(root_, writer);
}

bool ValueVisitor::VisitValue(const Json::Value& val, SerdeJsonWriter& writer) {
  switch (val.type()) {
    case Json::nullValue:
      write_null(writer);
      return true;
    case Json::intValue:
      write_i64(writer, val.asInt64());
      return true;
    case Json::uintValue:
      write_u64(writer, val.asUInt64());
      return true;
    case Json::realValue:
      write_f64(writer, val.asDouble());
      return true;
    case Json::stringValue: {
      const char* begin = nullptr;
      const char* end = nullptr;
      val.getString(&begin, &end);
      write_str(writer, ::rust::Str(begin, end - begin));
      return true;
    }
    case Json::booleanValue:
      write_bool(writer, val.asBool());
      return true;
    case Json::arrayValue: {
      write_array_start(writer);
      for (Json::ArrayIndex i = 0; i < val.size(); ++i) {
        if (!VisitValue(val[i], writer)) {
          return false;
        }
      }
      write_array_end(writer);
      return true;
    }
    case Json::objectValue: {
      write_object_start(writer);
      const auto member_names = val.getMemberNames();
      for (const auto& name : member_names) {
        write_object_key(writer, ::rust::Str(name.data(), name.size()));
        if (!VisitValue(val[name], writer)) {
          return false;
        }
      }
      write_object_end(writer);
      return true;
    }
  }
  return false;
}

}  // namespace serde
}  // namespace openscreen::json
