// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/cddl/codegen.h"

#include <cinttypes>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Convert '-' to '_' to use a CDDL identifier as a C identifier.
std::string ToUnderscoreId(const std::string& x) {
  std::string result(x);
  for (auto& c : result) {
    if (c == '-')
      c = '_';
  }
  return result;
}

// Convert a CDDL identifier to camel case for use as a C typename.  E.g.
// presentation-connection-message to PresentationConnectionMessage.
std::string ToCamelCase(const std::string& x) {
  std::string result(x);
  result[0] = toupper(result[0]);
  size_t new_size = 1;
  size_t result_size = result.size();
  for (size_t i = 1; i < result_size; ++i, ++new_size) {
    if (result[i] == '-') {
      ++i;
      if (i < result_size)
        result[new_size] = toupper(result[i]);
    } else {
      result[new_size] = result[i];
    }
  }
  result.resize(new_size);
  return result;
}

// Returns a string which represents the C++ type of |cpp_type|.  Returns an
// empty string if there is no valid representation for |cpp_type| (e.g. a
// vector with an invalid element type).
std::string CppTypeToString(const CppType& cpp_type) {
  switch (cpp_type.which) {
    case CppType::Which::kUint64:
      return "uint64_t";
    case CppType::Which::kString:
      return "std::string";
    case CppType::Which::kVector: {
      std::string element_string =
          CppTypeToString(*cpp_type.vector_type.element_type);
      if (element_string.empty())
        return std::string();
      return std::string("std::vector<") + element_string + std::string(">");
    } break;
    case CppType::Which::kEnum:
      return ToCamelCase(cpp_type.name);
    case CppType::Which::kStruct:
      return ToCamelCase(cpp_type.name);
    case CppType::Which::kTaggedType:
      return CppTypeToString(*cpp_type.tagged_type.real_type);
    default:
      return std::string();
  }
}

// Write the C++ struct member definitions of every type in |members| to the
// file descriptor |fd|.
bool WriteStructMembers(
    int fd,
    const std::vector<std::pair<std::string, CppType*>>& members) {
  for (const auto& x : members) {
    std::string type_string;
    switch (x.second->which) {
      case CppType::Which::kStruct: {
        if (x.second->struct_type.key_type ==
            CppType::Struct::KeyType::kPlainGroup) {
          if (!WriteStructMembers(fd, x.second->struct_type.members))
            return false;
          continue;
        } else {
          type_string = ToCamelCase(x.first);
        }
      } break;
      case CppType::Which::kOptional: {
        // TODO(btolsch): Make this optional<T> when one lands.
        dprintf(fd, "  bool has_%s;\n", ToUnderscoreId(x.first).c_str());
        type_string = CppTypeToString(*x.second->optional_type);
      } break;
      case CppType::Which::kDiscriminatedUnion: {
        std::string cid = ToUnderscoreId(x.first);
        type_string = ToCamelCase(x.first);
        dprintf(fd, "  struct %s {\n", type_string.c_str());
        dprintf(fd, "    %s();\n    ~%s();\n\n", type_string.c_str(),
                type_string.c_str());
        dprintf(fd, "  enum class Which {\n");
        for (auto* union_member : x.second->discriminated_union.members) {
          switch (union_member->which) {
            case CppType::Which::kUint64:
              dprintf(fd, "    kUint64,\n");
              break;
            case CppType::Which::kString:
              dprintf(fd, "    kString,\n");
              break;
            case CppType::Which::kBytes:
              dprintf(fd, "    kBytes,\n");
              break;
            default:
              return false;
          }
        }
        dprintf(fd, "    kUninitialized,\n");
        dprintf(fd, "  } which;\n");
        dprintf(fd, "  union {\n");
        for (auto* union_member : x.second->discriminated_union.members) {
          switch (union_member->which) {
            case CppType::Which::kUint64:
              dprintf(fd, "    uint64_t uint;\n");
              break;
            case CppType::Which::kString:
              dprintf(fd, "    std::string str;\n");
              break;
            case CppType::Which::kBytes:
              dprintf(fd, "    std::vector<uint8_t> bytes;\n");
              break;
            default:
              return false;
          }
        }
        // NOTE: This member allows the union to be easily constructed in an
        // effectively uninitialized state.  Its value should never be used.
        dprintf(fd, "    bool placeholder_;\n");
        dprintf(fd, "  };\n");
        dprintf(fd, "  };\n");
      } break;
      default:
        type_string = CppTypeToString(*x.second);
        break;
    }
    if (type_string.empty())
      return false;
    dprintf(fd, "  %s %s;\n", type_string.c_str(),
            ToUnderscoreId(x.first).c_str());
  }
  return true;
}

// Writes a C++ type definition for |type| to the file descriptor |fd|.  This
// only generates a definition for enums and structs.
bool WriteTypeDefinition(int fd, const CppType& type) {
  switch (type.which) {
    case CppType::Which::kEnum: {
      dprintf(fd, "\nenum %s : uint64_t {\n", ToCamelCase(type.name).c_str());
      for (const auto& x : type.enum_type.members) {
        dprintf(fd, "  k%s = %" PRIu64 "ull,\n", ToCamelCase(x.first).c_str(),
                x.second);
      }
      dprintf(fd, "};\n");
    } break;
    case CppType::Which::kStruct: {
      dprintf(fd, "\nstruct %s {\n", ToCamelCase(type.name).c_str());
      if (!WriteStructMembers(fd, type.struct_type.members))
        return false;
      dprintf(fd, "};\n");
    } break;
    default:
      break;
  }
  return true;
}

// Ensures that any dependencies within |cpp_type| are written to the file
// descriptor |fd| before writing |cpp_type| to the file descriptor |fd|.  This
// is done by walking the tree of types defined by |cpp_type| (e.g. all the
// members for a struct).  |defs| contains the names of types that have already
// been written.  If a type hasn't been written and needs to be, its name will
// also be added to |defs|.
bool EnsureDependentTypeDefinitionsWritten(int fd,
                                           const CppType& cpp_type,
                                           std::set<std::string>* defs) {
  switch (cpp_type.which) {
    case CppType::Which::kVector: {
      return EnsureDependentTypeDefinitionsWritten(
          fd, *cpp_type.vector_type.element_type, defs);
    } break;
    case CppType::Which::kEnum: {
      if (defs->find(cpp_type.name) != defs->end())
        return true;
      for (const auto* x : cpp_type.enum_type.sub_members)
        if (!EnsureDependentTypeDefinitionsWritten(fd, *x, defs))
          return false;
      defs->emplace(cpp_type.name);
      WriteTypeDefinition(fd, cpp_type);
    } break;
    case CppType::Which::kStruct: {
      if (cpp_type.struct_type.key_type !=
          CppType::Struct::KeyType::kPlainGroup) {
        if (defs->find(cpp_type.name) != defs->end())
          return true;
        for (const auto& x : cpp_type.struct_type.members)
          if (!EnsureDependentTypeDefinitionsWritten(fd, *x.second, defs))
            return false;
        defs->emplace(cpp_type.name);
        WriteTypeDefinition(fd, cpp_type);
      }
    } break;
    case CppType::Which::kOptional: {
      return EnsureDependentTypeDefinitionsWritten(fd, *cpp_type.optional_type,
                                                   defs);
    } break;
    case CppType::Which::kDiscriminatedUnion: {
      for (const auto* x : cpp_type.discriminated_union.members)
        if (!EnsureDependentTypeDefinitionsWritten(fd, *x, defs))
          return false;
    } break;
    case CppType::Which::kTaggedType: {
      if (!EnsureDependentTypeDefinitionsWritten(
              fd, *cpp_type.tagged_type.real_type, defs)) {
        return false;
      }
    } break;
    default:
      break;
  }
  return true;
}

// Writes the type definition for every C++ type in |table|.  This function
// makes sure to write them in such an order that all type dependencies are
// written before they are need so the resulting text in the file descriptor
// |fd| will compile without modification.  For example, the following would be
// bad output:
//
// struct Foo {
//   Bar bar;
//   int x;
// };
//
// struct Bar {
//   int alpha;
// };
//
// This function ensures that Bar would be written sometime before Foo.
bool WriteTypeDefinitions(int fd, const CppSymbolTable& table) {
  std::set<std::string> defs;
  CppType* root_type = table.cpp_type_map.find(table.root_rule)->second;
  // NOTE: Currently encoding the type tag as a uint8_t.
  if (root_type->discriminated_union.members.size() >
      std::numeric_limits<uint8_t>::max()) {
    return false;
  }
  for (const auto* type : root_type->discriminated_union.members) {
    CppType* real_type = type->tagged_type.real_type;
    if (real_type->which != CppType::Which::kStruct ||
        real_type->struct_type.key_type ==
            CppType::Struct::KeyType::kPlainGroup) {
      return false;
    }
    if (!EnsureDependentTypeDefinitionsWritten(fd, *real_type, &defs))
      return false;
  }

  dprintf(fd, "\nenum class Type {\n");
  for (const auto* type : root_type->discriminated_union.members) {
    dprintf(fd, "    k%s,\n",
            ToCamelCase(type->tagged_type.real_type->name).c_str());
  }
  dprintf(fd, "};\n");
  return true;
}

// Writes the function prototypes for the encode and decode functions for each
// type in |table| to the file descriptor |fd|.
bool WriteFunctionDeclarations(int fd, const CppSymbolTable& table) {
  CppType* root_type = table.cpp_type_map.find(table.root_rule)->second;
  for (const auto* type : root_type->discriminated_union.members) {
    CppType* real_type = type->tagged_type.real_type;
    const auto& name = real_type->name;
    if (real_type->which != CppType::Which::kStruct ||
        real_type->struct_type.key_type ==
            CppType::Struct::KeyType::kPlainGroup) {
      return false;
    }
    std::string cpp_name = ToCamelCase(name);
    dprintf(fd, "\nbool Encode%s(\n", cpp_name.c_str());
    dprintf(fd, "    const %s& data,\n", cpp_name.c_str());
    dprintf(fd, "    CborEncodeBuffer* buffer);\n");
    dprintf(fd, "ssize_t Encode%s(\n", cpp_name.c_str());
    dprintf(fd, "    const %s& data,\n", cpp_name.c_str());
    dprintf(fd, "    uint8_t* buffer,\n    size_t length);\n");
    dprintf(fd, "ssize_t Decode%s(\n", cpp_name.c_str());
    dprintf(fd, "    const uint8_t* buffer,\n    size_t length,\n");
    dprintf(fd, "    %s* data);\n", cpp_name.c_str());
  }
  return true;
}

bool WriteMapEncoder(
    int fd,
    const std::string& name,
    const std::vector<std::pair<std::string, CppType*>>& members,
    const std::string& nested_type_scope,
    int encoder_depth = 1);
bool WriteArrayEncoder(
    int fd,
    const std::string& name,
    const std::vector<std::pair<std::string, CppType*>>& members,
    const std::string& nested_type_scope,
    int encoder_depth = 1);

// Writes the encoding function for the C++ type |cpp_type| to the file
// descriptor |fd|.  |name| is the C++ variable name that needs to be encoded.
// |nested_type_scope| is the closest C++ scope name (i.e. struct name), which
// may be used to access local enum constants.  |encoder_depth| is used to
// independently name independent cbor encoders that need to be created.
bool WriteEncoder(int fd,
                  const std::string& name,
                  const CppType& cpp_type,
                  const std::string& nested_type_scope,
                  int encoder_depth) {
  switch (cpp_type.which) {
    case CppType::Which::kStruct:
      if (cpp_type.struct_type.key_type == CppType::Struct::KeyType::kMap) {
        if (!WriteMapEncoder(fd, name, cpp_type.struct_type.members,
                             cpp_type.name, encoder_depth)) {
          return false;
        }
        return true;
      } else if (cpp_type.struct_type.key_type ==
                 CppType::Struct::KeyType::kArray) {
        if (!WriteArrayEncoder(fd, name, cpp_type.struct_type.members,
                               cpp_type.name, encoder_depth)) {
          return false;
        }
        return true;
      } else {
        for (const auto& x : cpp_type.struct_type.members) {
          dprintf(fd,
                  "  CBOR_RETURN_ON_ERROR(cbor_encode_text_string(&encoder%d, "
                  "\"%s\", sizeof(\"%s\") - 1));\n",
                  encoder_depth, x.first.c_str(), x.first.c_str());
          if (!WriteEncoder(fd,
                            name + std::string(".") + ToUnderscoreId(x.first),
                            *x.second, nested_type_scope, encoder_depth)) {
            return false;
          }
        }
        return true;
      }
      break;
    case CppType::Which::kUint64:
      dprintf(fd, "  CBOR_RETURN_ON_ERROR(cbor_encode_uint(&encoder%d, %s));\n",
              encoder_depth, ToUnderscoreId(name).c_str());
      return true;
      break;
    case CppType::Which::kString: {
      std::string cid = ToUnderscoreId(name);
      dprintf(fd, "  if (!IsValidUtf8(%s)) {\n", cid.c_str());
      dprintf(fd, "    return -CborErrorInvalidUtf8TextString;\n");
      dprintf(fd, "  }\n");
      dprintf(fd,
              "  CBOR_RETURN_ON_ERROR(cbor_encode_text_string(&encoder%d, "
              "%s.c_str(), %s.size()));\n",
              encoder_depth, cid.c_str(), cid.c_str());
      return true;
    } break;
    case CppType::Which::kBytes: {
      std::string cid = ToUnderscoreId(name);
      dprintf(fd,
              "  CBOR_RETURN_ON_ERROR(cbor_encode_byte_string(&encoder%d, "
              "%s.data(), "
              "%s.size()));\n",
              encoder_depth, cid.c_str(), cid.c_str());
      return true;
    } break;
    case CppType::Which::kVector: {
      std::string cid = ToUnderscoreId(name);
      dprintf(fd, "  CborEncoder encoder%d;\n", encoder_depth + 1);
      dprintf(fd,
              "  CBOR_RETURN_ON_ERROR(cbor_encoder_create_array(&encoder%d, "
              "&encoder%d, %s.size()));\n",
              encoder_depth, encoder_depth + 1, cid.c_str());
      dprintf(fd, "  for (const auto& x : %s) {\n", cid.c_str());
      if (!WriteEncoder(fd, "x", *cpp_type.vector_type.element_type,
                        nested_type_scope, encoder_depth + 1)) {
        return false;
      }
      dprintf(fd, "  }\n");
      dprintf(fd,
              "  CBOR_RETURN_ON_ERROR(cbor_encoder_close_container(&encoder%d, "
              "&encoder%d));\n",
              encoder_depth, encoder_depth + 1);
      return true;
    } break;
    case CppType::Which::kEnum: {
      dprintf(fd, "  CBOR_RETURN_ON_ERROR(cbor_encode_uint(&encoder%d, %s));\n",
              encoder_depth, ToUnderscoreId(name).c_str());
      return true;
    } break;
    case CppType::Which::kDiscriminatedUnion: {
      for (const auto* union_member : cpp_type.discriminated_union.members) {
        switch (union_member->which) {
          case CppType::Which::kUint64:
            dprintf(fd, "  case %s::%s::Which::kUint64:\n",
                    ToCamelCase(nested_type_scope).c_str(),
                    ToCamelCase(cpp_type.name).c_str());
            if (!WriteEncoder(fd, ToUnderscoreId(name + std::string(".uint")),
                              *union_member, nested_type_scope,
                              encoder_depth)) {
              return false;
            }
            dprintf(fd, "    break;\n");
            break;
          case CppType::Which::kString:
            dprintf(fd, "  case %s::%s::Which::kString:\n",
                    ToCamelCase(nested_type_scope).c_str(),
                    ToCamelCase(cpp_type.name).c_str());
            if (!WriteEncoder(fd, ToUnderscoreId(name + std::string(".str")),
                              *union_member, nested_type_scope,
                              encoder_depth)) {
              return false;
            }
            dprintf(fd, "    break;\n");
            break;
          case CppType::Which::kBytes:
            dprintf(fd, "  case %s::%s::Which::kBytes:\n",
                    ToCamelCase(nested_type_scope).c_str(),
                    ToCamelCase(cpp_type.name).c_str());
            if (!WriteEncoder(fd, ToUnderscoreId(name + std::string(".bytes")),
                              *union_member, nested_type_scope,
                              encoder_depth)) {
              return false;
            }
            dprintf(fd, "    break;\n");
            break;
          default:
            return false;
        }
      }
      dprintf(fd, "  case %s::%s::Which::kUninitialized:\n",
              ToCamelCase(nested_type_scope).c_str(),
              ToCamelCase(cpp_type.name).c_str());
      dprintf(fd, "    return -CborUnknownError;\n");
      return true;
    } break;
    case CppType::Which::kTaggedType: {
      dprintf(fd,
              "  CBOR_RETURN_ON_ERROR(cbor_encode_tag(&encoder%d, %" PRIu64
              "ull));\n",
              encoder_depth, cpp_type.tagged_type.tag);
      if (!WriteEncoder(fd, name, *cpp_type.tagged_type.real_type,
                        nested_type_scope, encoder_depth)) {
        return false;
      }
      return true;
    } break;
    default:
      break;
  }
  return false;
}

struct MemberCountResult {
  int num_required;
  int num_optional;
};

MemberCountResult CountMemberTypes(
    int fd,
    const std::string& name_id,
    const std::vector<std::pair<std::string, CppType*>>& members) {
  int num_required = 0;
  int num_optional = 0;
  for (const auto& x : members) {
    if (x.second->which == CppType::Which::kOptional) {
      std::string x_id = ToUnderscoreId(x.first);
      if (num_optional == 0) {
        dprintf(fd, "  int num_optionals_present = %s.has_%s;\n",
                name_id.c_str(), x_id.c_str());
      } else {
        dprintf(fd, "  num_optionals_present += %s.has_%s;\n", name_id.c_str(),
                x_id.c_str());
      }
      ++num_optional;
    } else {
      ++num_required;
    }
  }
  return MemberCountResult{num_required, num_optional};
}

// Writes the encoding function for a CBOR map with the C++ type members in
// |members| to the file descriptor |fd|.  |name| is the C++ variable name that
// needs to be encoded.  |nested_type_scope| is the closest C++ scope name (i.e.
// struct name), which may be used to access local enum constants.
// |encoder_depth| is used to independently name independent cbor encoders that
// need to be created.
bool WriteMapEncoder(
    int fd,
    const std::string& name,
    const std::vector<std::pair<std::string, CppType*>>& members,
    const std::string& nested_type_scope,
    int encoder_depth) {
  std::string name_id = ToUnderscoreId(name);
  dprintf(fd, "  CborEncoder encoder%d;\n", encoder_depth);
  MemberCountResult member_counts = CountMemberTypes(fd, name_id, members);
  if (member_counts.num_optional == 0) {
    dprintf(fd,
            "  CBOR_RETURN_ON_ERROR(cbor_encoder_create_map(&encoder%d, "
            "&encoder%d, "
            "%d));\n",
            encoder_depth - 1, encoder_depth, member_counts.num_required);
  } else {
    dprintf(fd,
            "  CBOR_RETURN_ON_ERROR(cbor_encoder_create_map(&encoder%d, "
            "&encoder%d, "
            "%d + num_optionals_present));\n",
            encoder_depth - 1, encoder_depth, member_counts.num_required);
  }

  for (const auto& x : members) {
    std::string fullname = name;
    CppType* member_type = x.second;
    if (x.second->which != CppType::Which::kStruct ||
        x.second->struct_type.key_type !=
            CppType::Struct::KeyType::kPlainGroup) {
      if (x.second->which == CppType::Which::kOptional) {
        member_type = x.second->optional_type;
        dprintf(fd, "  if (%s.has_%s) {\n", name_id.c_str(),
                ToUnderscoreId(x.first).c_str());
      }
      dprintf(
          fd,
          "  CBOR_RETURN_ON_ERROR(cbor_encode_text_string(&encoder%d, \"%s\", "
          "sizeof(\"%s\") - 1));\n",
          encoder_depth, x.first.c_str(), x.first.c_str());
      if (x.second->which == CppType::Which::kDiscriminatedUnion) {
        dprintf(fd, "  switch (%s.%s.which) {\n", fullname.c_str(),
                x.first.c_str());
      }
      fullname = fullname + std::string(".") + x.first;
    }
    if (!WriteEncoder(fd, fullname, *member_type, nested_type_scope,
                      encoder_depth)) {
      return false;
    }
    if (x.second->which == CppType::Which::kOptional ||
        x.second->which == CppType::Which::kDiscriminatedUnion) {
      dprintf(fd, "  }\n");
    }
  }

  dprintf(fd,
          "  CBOR_RETURN_ON_ERROR(cbor_encoder_close_container(&encoder%d, "
          "&encoder%d));\n",
          encoder_depth - 1, encoder_depth);
  return true;
}

// Writes the encoding function for a CBOR array with the C++ type members in
// |members| to the file descriptor |fd|.  |name| is the C++ variable name that
// needs to be encoded.  |nested_type_scope| is the closest C++ scope name (i.e.
// struct name), which may be used to access local enum constants.
// |encoder_depth| is used to independently name independent cbor encoders that
// need to be created.
bool WriteArrayEncoder(
    int fd,
    const std::string& name,
    const std::vector<std::pair<std::string, CppType*>>& members,
    const std::string& nested_type_scope,
    int encoder_depth) {
  std::string name_id = ToUnderscoreId(name);
  dprintf(fd, "  CborEncoder encoder%d;\n", encoder_depth);
  MemberCountResult member_counts = CountMemberTypes(fd, name_id, members);
  if (member_counts.num_optional == 0) {
    dprintf(fd,
            "  CBOR_RETURN_ON_ERROR(cbor_encoder_create_array(&encoder%d, "
            "&encoder%d, %d));\n",
            encoder_depth - 1, encoder_depth, member_counts.num_required);
  } else {
    dprintf(fd,
            "  CBOR_RETURN_ON_ERROR(cbor_encoder_create_array(&encoder%d, "
            "&encoder%d, %d + num_optionals_present));\n",
            encoder_depth - 1, encoder_depth, member_counts.num_required);
  }

  for (const auto& x : members) {
    std::string fullname = name;
    CppType* member_type = x.second;
    if (x.second->which != CppType::Which::kStruct ||
        x.second->struct_type.key_type !=
            CppType::Struct::KeyType::kPlainGroup) {
      if (x.second->which == CppType::Which::kOptional) {
        member_type = x.second->optional_type;
        dprintf(fd, "  if (%s.has_%s) {\n", name_id.c_str(),
                ToUnderscoreId(x.first).c_str());
      }
      if (x.second->which == CppType::Which::kDiscriminatedUnion) {
        dprintf(fd, "  switch (%s.%s.which) {\n", fullname.c_str(),
                x.first.c_str());
      }
      fullname = fullname + std::string(".") + x.first;
    }
    if (!WriteEncoder(fd, fullname, *member_type, nested_type_scope,
                      encoder_depth)) {
      return false;
    }
    if (x.second->which == CppType::Which::kOptional ||
        x.second->which == CppType::Which::kDiscriminatedUnion) {
      dprintf(fd, "  }\n");
    }
  }

  dprintf(fd,
          "  CBOR_RETURN_ON_ERROR(cbor_encoder_close_container(&encoder%d, "
          "&encoder%d));\n",
          encoder_depth - 1, encoder_depth);
  return true;
}

// Writes encoding functions for each type in |table| to the file descriptor
// |fd|.
bool WriteEncoders(int fd, const CppSymbolTable& table) {
  CppType* root_type = table.cpp_type_map.find(table.root_rule)->second;
  for (const auto* type : root_type->discriminated_union.members) {
    CppType* real_type = type->tagged_type.real_type;
    const auto& name = real_type->name;
    if (real_type->which != CppType::Which::kStruct ||
        real_type->struct_type.key_type ==
            CppType::Struct::KeyType::kPlainGroup) {
      return false;
    }
    std::string cpp_name = ToCamelCase(name);

    for (const auto& x : real_type->struct_type.members) {
      if (x.second->which != CppType::Which::kDiscriminatedUnion)
        continue;
      std::string dunion_cpp_name = ToCamelCase(x.first);
      dprintf(fd, "\n%s::%s::%s()\n", cpp_name.c_str(), dunion_cpp_name.c_str(),
              dunion_cpp_name.c_str());
      std::string cid = ToUnderscoreId(x.first);
      std::string type_name = ToCamelCase(x.first);
      dprintf(fd,
              "    : which(Which::kUninitialized), placeholder_(false) {}\n");

      dprintf(fd, "\n%s::%s::~%s() {\n", cpp_name.c_str(),
              dunion_cpp_name.c_str(), dunion_cpp_name.c_str());
      dprintf(fd, "  switch (which) {\n");
      for (const auto* y : x.second->discriminated_union.members) {
        switch (y->which) {
          case CppType::Which::kUint64: {
            dprintf(fd, " case Which::kUint64: break;\n");
          } break;
          case CppType::Which::kString: {
            dprintf(fd, "  case Which::kString:\n");
            dprintf(fd, "    str.std::string::~basic_string();\n");
            dprintf(fd, "    break;\n");
          } break;
          case CppType::Which::kBytes: {
            dprintf(fd, "  case Which::kBytes:\n");
            dprintf(fd, "    bytes.std::vector<uint8_t>::~vector();\n");
            dprintf(fd, "    break;\n");
          } break;
          default:
            return false;
        }
      }
      dprintf(fd, " case Which::kUninitialized: break;\n");
      dprintf(fd, "  }\n");
      dprintf(fd, "}\n");
    }

    static const char vector_encode_function[] =
        R"(
bool Encode%1$s(
    const %1$s& data,
    CborEncodeBuffer* buffer) {
  if (buffer->AvailableLength() == 0 &&
      !buffer->Append(CborEncodeBuffer::kDefaultInitialEncodeBufferSize))
    return false;
  buffer->SetType(Type::k%1$s);
  while (true) {
    size_t available_length = buffer->AvailableLength();
    ssize_t error_or_size = msgs::Encode%1$s(
        data, buffer->Position(), available_length);
    if (IsError(error_or_size)) {
      return false;
    } else if (error_or_size > static_cast<ssize_t>(available_length)) {
      if (!buffer->ResizeBy(error_or_size - available_length))
        return false;
    } else {
      buffer->ResizeBy(error_or_size - available_length);
      return true;
    }
  }
}
)";

    dprintf(fd, vector_encode_function, cpp_name.c_str());

    dprintf(fd, "\nssize_t Encode%s(\n", cpp_name.c_str());
    dprintf(fd, "    const %s& data,\n", cpp_name.c_str());
    dprintf(fd, "    uint8_t* buffer,\n    size_t length) {\n");
    dprintf(fd, "  CborEncoder encoder0;\n");
    dprintf(fd, "  cbor_encoder_init(&encoder0, buffer, length, 0);\n");

    if (real_type->struct_type.key_type == CppType::Struct::KeyType::kMap) {
      if (!WriteMapEncoder(fd, "data", real_type->struct_type.members, name))
        return false;
    } else {
      if (!WriteArrayEncoder(fd, "data", real_type->struct_type.members,
                             name)) {
        return false;
      }
    }

    dprintf(fd,
            "  size_t extra_bytes_needed = "
            "cbor_encoder_get_extra_bytes_needed(&encoder0);\n");
    dprintf(fd, "  if (extra_bytes_needed) {\n");
    dprintf(fd,
            "    return static_cast<ssize_t>(length + extra_bytes_needed);\n");
    dprintf(fd, "  } else {\n");
    dprintf(fd,
            "    return "
            "static_cast<ssize_t>(cbor_encoder_get_buffer_size(&encoder0, "
            "buffer));\n");
    dprintf(fd, "  }\n");
    dprintf(fd, "}\n");
  }
  return true;
}

bool WriteMapDecoder(
    int fd,
    const std::string& name,
    const std::string& member_accessor,
    const std::vector<std::pair<std::string, CppType*>>& members,
    int decoder_depth,
    int* temporary_count);
bool WriteArrayDecoder(
    int fd,
    const std::string& name,
    const std::string& member_accessor,
    const std::vector<std::pair<std::string, CppType*>>& members,
    int decoder_depth,
    int* temporary_count);

// Writes the decoding function for the C++ type |cpp_type| to the file
// descriptor |fd|.  |name| is the C++ variable name that needs to be encoded.
// |member_accessor| is either "." or "->" depending on whether |name| is a
// pointer type.  |decoder_depth| is used to independently name independent cbor
// decoders that need to be created.  |temporary_count| is used to ensure
// temporaries get unique names by appending an automatically incremented
// integer.
bool WriteDecoder(int fd,
                  const std::string& name,
                  const std::string& member_accessor,
                  const CppType& cpp_type,
                  int decoder_depth,
                  int* temporary_count) {
  switch (cpp_type.which) {
    case CppType::Which::kUint64: {
      dprintf(fd,
              "  CBOR_RETURN_ON_ERROR(cbor_value_get_uint64(&it%d, &%s));\n",
              decoder_depth, name.c_str());
      dprintf(fd, "  CBOR_RETURN_ON_ERROR(cbor_value_advance_fixed(&it%d));\n",
              decoder_depth);
      return true;
    } break;
    case CppType::Which::kString: {
      int temp_length = (*temporary_count)++;
      dprintf(fd, "  size_t length%d = 0;", temp_length);
      dprintf(fd,
              "  CBOR_RETURN_ON_ERROR(cbor_value_validate(&it%d, "
              "CborValidateUtf8));\n",
              decoder_depth);
      dprintf(fd, "  if (cbor_value_is_length_known(&it%d)) {\n",
              decoder_depth);
      dprintf(fd,
              "    CBOR_RETURN_ON_ERROR(cbor_value_get_string_length(&it%d, "
              "&length%d));\n",
              decoder_depth, temp_length);
      dprintf(fd, "  } else {\n");
      dprintf(
          fd,
          "    CBOR_RETURN_ON_ERROR(cbor_value_calculate_string_length(&it%d, "
          "&length%d));\n",
          decoder_depth, temp_length);
      dprintf(fd, "  }\n");
      dprintf(fd, "  %s%sresize(length%d);\n", name.c_str(),
              member_accessor.c_str(), temp_length);
      dprintf(fd,
              "  CBOR_RETURN_ON_ERROR(cbor_value_copy_text_string(&it%d, "
              "const_cast<char*>(%s%sdata()), &length%d, nullptr));\n",
              decoder_depth, name.c_str(), member_accessor.c_str(),
              temp_length);
      dprintf(fd, "  CBOR_RETURN_ON_ERROR(cbor_value_advance(&it%d));\n",
              decoder_depth);
      return true;
    } break;
    case CppType::Which::kBytes: {
      int temp_length = (*temporary_count)++;
      dprintf(fd, "  size_t length%d = 0;", temp_length);
      dprintf(fd, "  if (cbor_value_is_length_known(&it%d)) {\n",
              decoder_depth);
      dprintf(fd,
              "    CBOR_RETURN_ON_ERROR(cbor_value_get_string_length(&it%d, "
              "&length%d));\n",
              decoder_depth, temp_length);
      dprintf(fd, "  } else {\n");
      dprintf(
          fd,
          "    CBOR_RETURN_ON_ERROR(cbor_value_calculate_string_length(&it%d, "
          "&length%d));\n",
          decoder_depth, temp_length);
      dprintf(fd, "  }\n");
      dprintf(fd, "  %s%sresize(length%d);\n", name.c_str(),
              member_accessor.c_str(), temp_length);
      dprintf(fd,
              "  CBOR_RETURN_ON_ERROR(cbor_value_copy_byte_string(&it%d, "
              "const_cast<uint8_t*>(%s%sdata()), &length%d, nullptr));\n",
              decoder_depth, name.c_str(), member_accessor.c_str(),
              temp_length);
      dprintf(fd, "  CBOR_RETURN_ON_ERROR(cbor_value_advance(&it%d));\n",
              decoder_depth);
      return true;
    } break;
    case CppType::Which::kVector: {
      dprintf(fd, "  if (cbor_value_get_type(&it%d) != CborArrayType) {\n",
              decoder_depth);
      dprintf(fd, "    return -1;\n");
      dprintf(fd, "  }\n");
      dprintf(fd, "  CborValue it%d;\n", decoder_depth + 1);
      dprintf(fd, "  size_t it%d_length = 0;\n", decoder_depth + 1);
      dprintf(fd,
              "  CBOR_RETURN_ON_ERROR(cbor_value_get_array_length(&it%d, "
              "&it%d_length));\n",
              decoder_depth, decoder_depth + 1);
      dprintf(fd, "  %s%sresize(it%d_length);\n", name.c_str(),
              member_accessor.c_str(), decoder_depth + 1);
      dprintf(
          fd,
          "  CBOR_RETURN_ON_ERROR(cbor_value_enter_container(&it%d, &it%d));\n",
          decoder_depth, decoder_depth + 1);
      dprintf(fd, "  for (auto i = %s%sbegin(); i != %s%send(); ++i) {\n",
              name.c_str(), member_accessor.c_str(), name.c_str(),
              member_accessor.c_str());
      if (!WriteDecoder(fd, "(*i)", ".", *cpp_type.vector_type.element_type,
                        decoder_depth + 1, temporary_count)) {
        return false;
      }
      dprintf(fd, "  }\n");
      dprintf(
          fd,
          "  CBOR_RETURN_ON_ERROR(cbor_value_leave_container(&it%d, &it%d));\n",
          decoder_depth, decoder_depth + 1);
      return true;
    } break;
    case CppType::Which::kEnum: {
      dprintf(fd,
              "  CBOR_RETURN_ON_ERROR(cbor_value_get_uint64(&it%d, "
              "reinterpret_cast<uint64_t*>(&%s)));\n",
              decoder_depth, name.c_str());
      dprintf(fd, "  CBOR_RETURN_ON_ERROR(cbor_value_advance_fixed(&it%d));\n",
              decoder_depth);
      // TODO(btolsch): Validate against enum members.
      return true;
    } break;
    case CppType::Which::kStruct: {
      if (cpp_type.struct_type.key_type == CppType::Struct::KeyType::kMap) {
        return WriteMapDecoder(fd, name, member_accessor,
                               cpp_type.struct_type.members, decoder_depth + 1,
                               temporary_count);
      } else if (cpp_type.struct_type.key_type ==
                 CppType::Struct::KeyType::kArray) {
        return WriteArrayDecoder(fd, name, member_accessor,
                                 cpp_type.struct_type.members,
                                 decoder_depth + 1, temporary_count);
      }
    } break;
    case CppType::Which::kDiscriminatedUnion: {
      int temp_value_type = (*temporary_count)++;
      dprintf(fd, "  CborType type%d = cbor_value_get_type(&it%d);\n",
              temp_value_type, decoder_depth);
      bool first = true;
      for (const auto* x : cpp_type.discriminated_union.members) {
        if (first)
          first = false;
        else
          dprintf(fd, " else ");
        switch (x->which) {
          case CppType::Which::kUint64:
            dprintf(fd,
                    "  if (type%d == CborIntegerType && (it%d.flags & "
                    "CborIteratorFlag_NegativeInteger) == 0) {\n",
                    temp_value_type, decoder_depth);
            dprintf(fd, "  %s.which = decltype(%s)::Which::kUint64;\n",
                    name.c_str(), name.c_str());
            if (!WriteDecoder(fd, name + std::string(".uint"), ".", *x,
                              decoder_depth, temporary_count)) {
              return false;
            }
            break;
          case CppType::Which::kString: {
            dprintf(fd, "  if (type%d == CborTextStringType) {\n",
                    temp_value_type);
            dprintf(fd, "  %s.which = decltype(%s)::Which::kString;\n",
                    name.c_str(), name.c_str());
            std::string str_name = name + std::string(".str");
            dprintf(fd, "  new (&%s) std::string();\n", str_name.c_str());
            if (!WriteDecoder(fd, str_name, ".", *x, decoder_depth,
                              temporary_count)) {
              return false;
            }
          } break;
          case CppType::Which::kBytes: {
            dprintf(fd, "  if (type%d == CborByteStringType) {\n",
                    temp_value_type);
            std::string bytes_name = name + std::string(".bytes");
            dprintf(fd, "  %s.which = decltype(%s)::Which::kBytes;\n",
                    name.c_str(), name.c_str());
            dprintf(fd, "  new (&%s) std::vector<uint8_t>();\n",
                    bytes_name.c_str());
            if (!WriteDecoder(fd, bytes_name, ".", *x, decoder_depth,
                              temporary_count)) {
              return false;
            }
          } break;
          default:
            return false;
        }
        dprintf(fd, "  }\n");
      }
      dprintf(fd, " else { return -1; }\n");
      return true;
    } break;
    case CppType::Which::kTaggedType: {
      int temp_tag = (*temporary_count)++;
      dprintf(fd, "  uint64_t tag%d = 0;\n", temp_tag);
      dprintf(fd, "  cbor_value_get_tag(&it%d, &tag%d);\n", decoder_depth,
              temp_tag);
      dprintf(fd, "  if (tag%d != %" PRIu64 "ull) {\n", temp_tag,
              cpp_type.tagged_type.tag);
      dprintf(fd, "    return -1;\n");
      dprintf(fd, "  }\n");
      dprintf(fd, "  CBOR_RETURN_ON_ERROR(cbor_value_advance_fixed(&it%d));\n",
              decoder_depth);
      if (!WriteDecoder(fd, name, member_accessor,
                        *cpp_type.tagged_type.real_type, decoder_depth,
                        temporary_count)) {
        return false;
      }
      return true;
    } break;
    default:
      break;
  }
  return false;
}

// Writes the decoding function for the CBOR map with members in |members| to
// the file descriptor |fd|.  |name| is the C++ variable name that needs to be
// encoded.  |member_accessor| is either "." or "->" depending on whether |name|
// is a pointer type.  |decoder_depth| is used to independently name independent
// cbor decoders that need to be created.  |temporary_count| is used to ensure
// temporaries get unique names by appending an automatically incremented
// integer.
bool WriteMapDecoder(
    int fd,
    const std::string& name,
    const std::string& member_accessor,
    const std::vector<std::pair<std::string, CppType*>>& members,
    int decoder_depth,
    int* temporary_count) {
  dprintf(fd, "  if (cbor_value_get_type(&it%d) != CborMapType) {\n",
          decoder_depth - 1);
  dprintf(fd, "    return -1;\n");
  dprintf(fd, "  }\n");
  dprintf(fd, "  CborValue it%d;\n", decoder_depth);
  dprintf(fd, "  size_t it%d_length = 0;\n", decoder_depth);
  dprintf(fd,
          "  CBOR_RETURN_ON_ERROR(cbor_value_get_map_length(&it%d, "
          "&it%d_length));\n",
          decoder_depth - 1, decoder_depth);
  int optional_members = 0;
  for (const auto& member : members) {
    if (member.second->which == CppType::Which::kOptional) {
      ++optional_members;
    }
  }
  dprintf(fd, "  if (it%d_length != %d", decoder_depth,
          static_cast<int>(members.size()));
  for (int i = 0; i < optional_members; ++i) {
    dprintf(fd, " && it%d_length != %d", decoder_depth,
            static_cast<int>(members.size()) - i - 1);
  }
  dprintf(fd, ") {\n");
  dprintf(fd, "    return -1;\n");
  dprintf(fd, "  }\n");
  dprintf(fd,
          "  CBOR_RETURN_ON_ERROR(cbor_value_enter_container(&it%d, &it%d));\n",
          decoder_depth - 1, decoder_depth);
  int member_pos = 0;
  for (const auto& x : members) {
    std::string cid = ToUnderscoreId(x.first);
    std::string fullname = name + member_accessor + cid;
    if (x.second->which == CppType::Which::kOptional) {
      // TODO(btolsch): This is wrong for the same reason as arrays, but will be
      // easier to handle when doing out-of-order keys.
      dprintf(fd, "  if (it%d_length > %d) {\n", decoder_depth, member_pos);
      dprintf(fd,
              "  CBOR_RETURN_ON_ERROR(EXPECT_KEY_CONSTANT(&it%d, \"%s\"));\n",
              decoder_depth, x.first.c_str());
      dprintf(fd, "    %s%shas_%s = true;\n", name.c_str(),
              member_accessor.c_str(), cid.c_str());
      if (!WriteDecoder(fd, fullname, ".", *x.second->optional_type,
                        decoder_depth, temporary_count)) {
        return false;
      }
      dprintf(fd, "  } else {\n");
      dprintf(fd, "    %s%shas_%s = false;\n", name.c_str(),
              member_accessor.c_str(), cid.c_str());
      dprintf(fd, "  }\n");
    } else {
      dprintf(fd,
              "  CBOR_RETURN_ON_ERROR(EXPECT_KEY_CONSTANT(&it%d, \"%s\"));\n",
              decoder_depth, x.first.c_str());
      if (!WriteDecoder(fd, fullname, ".", *x.second, decoder_depth,
                        temporary_count)) {
        return false;
      }
    }
    ++member_pos;
  }
  dprintf(fd,
          "  CBOR_RETURN_ON_ERROR(cbor_value_leave_container(&it%d, &it%d));\n",
          decoder_depth - 1, decoder_depth);
  return true;
}

// Writes the decoding function for the CBOR array with members in |members| to
// the file descriptor |fd|.  |name| is the C++ variable name that needs to be
// encoded.  |member_accessor| is either "." or "->" depending on whether |name|
// is a pointer type.  |decoder_depth| is used to independently name independent
// cbor decoders that need to be created.  |temporary_count| is used to ensure
// temporaries get unique names by appending an automatically incremented
// integer.
bool WriteArrayDecoder(
    int fd,
    const std::string& name,
    const std::string& member_accessor,
    const std::vector<std::pair<std::string, CppType*>>& members,
    int decoder_depth,
    int* temporary_count) {
  dprintf(fd, "  if (cbor_value_get_type(&it%d) != CborArrayType) {\n",
          decoder_depth - 1);
  dprintf(fd, "    return -1;\n");
  dprintf(fd, "  }\n");
  dprintf(fd, "  CborValue it%d;\n", decoder_depth);
  dprintf(fd, "  size_t it%d_length = 0;\n", decoder_depth);
  dprintf(fd,
          "  CBOR_RETURN_ON_ERROR(cbor_value_get_array_length(&it%d, "
          "&it%d_length));\n",
          decoder_depth - 1, decoder_depth);
  int optional_members = 0;
  for (const auto& member : members) {
    if (member.second->which == CppType::Which::kOptional) {
      ++optional_members;
    }
  }
  dprintf(fd, "  if (it%d_length != %d", decoder_depth,
          static_cast<int>(members.size()));
  for (int i = 0; i < optional_members; ++i) {
    dprintf(fd, " && it%d_length != %d", decoder_depth,
            static_cast<int>(members.size()) - i - 1);
  }
  dprintf(fd, ") {\n");
  dprintf(fd, "    return -1;\n");
  dprintf(fd, "  }\n");
  dprintf(fd,
          "  CBOR_RETURN_ON_ERROR(cbor_value_enter_container(&it%d, &it%d));\n",
          decoder_depth - 1, decoder_depth);
  int member_pos = 0;
  for (const auto& x : members) {
    std::string cid = ToUnderscoreId(x.first);
    std::string fullname = name + member_accessor + cid;
    if (x.second->which == CppType::Which::kOptional) {
      // TODO(btolsch): This only handles a single block of optionals and only
      // the ones present form a contiguous range from the start of the block.
      // However, we likely don't really need more than one optional for arrays
      // for the foreseeable future.  The proper approach would be to have a set
      // of possible types for the next element and a map for the member to
      // which each corresponds.
      dprintf(fd, "  if (it%d_length > %d) {\n", decoder_depth, member_pos);
      dprintf(fd, "    %s%shas_%s = true;\n", name.c_str(),
              member_accessor.c_str(), cid.c_str());
      if (!WriteDecoder(fd, fullname, ".", *x.second->optional_type,
                        decoder_depth, temporary_count)) {
        return false;
      }
      dprintf(fd, "  } else {\n");
      dprintf(fd, "    %s%shas_%s = false;\n", name.c_str(),
              member_accessor.c_str(), cid.c_str());
      dprintf(fd, "  }\n");
    } else {
      if (!WriteDecoder(fd, fullname, ".", *x.second, decoder_depth,
                        temporary_count)) {
        return false;
      }
    }
    ++member_pos;
  }
  dprintf(fd,
          "  CBOR_RETURN_ON_ERROR(cbor_value_leave_container(&it%d, &it%d));\n",
          decoder_depth - 1, decoder_depth);
  return true;
}

// Writes a decoder function definition for every type in |table| to the file
// descriptor |fd|.
bool WriteDecoders(int fd, const CppSymbolTable& table) {
  CppType* root_type = table.cpp_type_map.find(table.root_rule)->second;
  for (const auto* type : root_type->discriminated_union.members) {
    CppType* real_type = type->tagged_type.real_type;
    const auto& name = real_type->name;
    int temporary_count = 0;
    if (real_type->which != CppType::Which::kStruct ||
        real_type->struct_type.key_type ==
            CppType::Struct::KeyType::kPlainGroup) {
      continue;
    }
    std::string cpp_name = ToCamelCase(name);
    dprintf(fd, "\nssize_t Decode%s(\n", cpp_name.c_str());
    dprintf(fd, "    const uint8_t* buffer,\n    size_t length,\n");
    dprintf(fd, "    %s* data) {\n", cpp_name.c_str());
    dprintf(fd, "  CborParser parser;\n");
    dprintf(fd, "  CborValue it0;\n");
    dprintf(
        fd,
        "  CBOR_RETURN_ON_ERROR(cbor_parser_init(buffer, length, 0, &parser, "
        "&it0));\n");
    if (real_type->struct_type.key_type == CppType::Struct::KeyType::kMap) {
      if (!WriteMapDecoder(fd, "data", "->", real_type->struct_type.members, 1,
                           &temporary_count)) {
        return false;
      }
    } else {
      if (!WriteArrayDecoder(fd, "data", "->", real_type->struct_type.members,
                             1, &temporary_count)) {
        return false;
      }
    }
    dprintf(
        fd,
        "  auto result = static_cast<ssize_t>(cbor_value_get_next_byte(&it0) - "
        "buffer);\n");
    dprintf(fd, "  return result;\n");
    dprintf(fd, "}\n");
  }
  return true;
}

// Converts the filename |header_filename| to a preprocessor token that can be
// used as a header guard macro name.
std::string ToHeaderGuard(const std::string& header_filename) {
  std::string result = header_filename;
  for (auto& c : result) {
    if (c == '/' || c == '.')
      c = '_';
    else
      c = toupper(c);
  }
  result += "_";
  return result;
}

bool WriteHeaderPrologue(int fd, const std::string& header_filename) {
  static const char prologue[] =
      R"(#ifndef %s
#define %s

#include <cstdint>
#include <string>
#include <vector>

namespace openscreen {
namespace msgs {

class CborEncodeBuffer;
)";
  std::string header_guard = ToHeaderGuard(header_filename);
  dprintf(fd, prologue, header_guard.c_str(), header_guard.c_str());
  return true;
}

bool WriteHeaderEpilogue(int fd, const std::string& header_filename) {
  static const char epilogue[] = R"(
class CborEncodeBuffer {
 public:
  static constexpr size_t kDefaultInitialEncodeBufferSize = 250;
  static constexpr size_t kDefaultMaxEncodeBufferSize = 64000;

  CborEncodeBuffer();
  CborEncodeBuffer(size_t initial_size, size_t max_size);
  ~CborEncodeBuffer();

  bool Append(size_t length);
  bool ResizeBy(ssize_t length);
  void SetType(Type type);

  const uint8_t* data() const { return data_.data(); }
  size_t size() const { return data_.size(); }

  uint8_t* Position() { return &data_[0] + position_; }
  size_t AvailableLength() { return data_.size() - position_; }

 private:
  size_t max_size_;
  size_t position_;
  std::vector<uint8_t> data_;
};

}  // namespace msgs
}  // namespace openscreen
#endif  // %s)";
  std::string header_guard = ToHeaderGuard(header_filename);
  dprintf(fd, epilogue, header_guard.c_str());
  return true;
}

bool WriteSourcePrologue(int fd, const std::string& header_filename) {
  static const char prologue[] =
      R"(#include "%s"

#include "platform/api/logging.h"
#include "third_party/tinycbor/src/src/cbor.h"
#include "third_party/tinycbor/src/src/utf8_p.h"

namespace openscreen {
namespace msgs {
namespace {

#define CBOR_RETURN_WHAT_ON_ERROR(stmt, what)                           \
  {                                                                     \
    CborError error = stmt;                                             \
    /* Encoder-specific errors, so it's fine to check these even in the \
     * parser.                                                          \
     */                                                                 \
    OSP_DCHECK_NE(error, CborErrorTooFewItems);                             \
    OSP_DCHECK_NE(error, CborErrorTooManyItems);                            \
    OSP_DCHECK_NE(error, CborErrorDataTooLarge);                            \
    if (error != CborNoError && error != CborErrorOutOfMemory)          \
      return what;                                                      \
  }
#define CBOR_RETURN_ON_ERROR_INTERNAL(stmt) \
  CBOR_RETURN_WHAT_ON_ERROR(stmt, error)
#define CBOR_RETURN_ON_ERROR(stmt) CBOR_RETURN_WHAT_ON_ERROR(stmt, -error)

#define EXPECT_KEY_CONSTANT(it, key) ExpectKey(it, key, sizeof(key) - 1)

bool IsValidUtf8(const std::string& s) {
  const uint8_t* buffer = reinterpret_cast<const uint8_t*>(s.data());
  const uint8_t* end = buffer + s.size();
  while (buffer < end) {
    // TODO(btolsch): This is an implementation detail of tinycbor so we should
    // eventually replace this call with our own utf8 validation.
    if (get_utf8(&buffer, end) == ~0u)
      return false;
  }
  return true;
}

CborError ExpectKey(CborValue* it, const char* key, size_t key_length) {
  size_t observed_length = 0;
  CBOR_RETURN_ON_ERROR_INTERNAL(
      cbor_value_get_string_length(it, &observed_length));
  if (observed_length != key_length)
    return CborErrorImproperValue;
  std::string observed_key(key_length, 0);
  CBOR_RETURN_ON_ERROR_INTERNAL(cbor_value_copy_text_string(
      it, const_cast<char*>(observed_key.data()), &observed_length, nullptr));
  if (observed_key != key)
    return CborErrorImproperValue;
  CBOR_RETURN_ON_ERROR_INTERNAL(cbor_value_advance(it));
  return CborNoError;
}

}  // namespace

// static
constexpr size_t CborEncodeBuffer::kDefaultInitialEncodeBufferSize;

// static
constexpr size_t CborEncodeBuffer::kDefaultMaxEncodeBufferSize;

CborEncodeBuffer::CborEncodeBuffer()
    : max_size_(kDefaultMaxEncodeBufferSize),
      position_(1),
      data_(kDefaultInitialEncodeBufferSize) {}
CborEncodeBuffer::CborEncodeBuffer(size_t initial_size, size_t max_size)
    : max_size_(max_size), position_(1), data_(initial_size) {}
CborEncodeBuffer::~CborEncodeBuffer() = default;

bool CborEncodeBuffer::Append(size_t length) {
  if (length == 0)
    return false;
  if ((data_.size() + length) > max_size_) {
    length = max_size_ - data_.size();
    if (length == 0)
      return false;
  }
  size_t append_area = data_.size();
  data_.resize(append_area + length);
  position_ = append_area + 1;
  return true;
}

bool CborEncodeBuffer::ResizeBy(ssize_t delta) {
  if (delta == 0)
    return true;
  if (delta < 0 && static_cast<size_t>(-delta) > data_.size())
    return false;
  if (delta > 0 && (data_.size() + delta) > max_size_)
    return false;
  data_.resize(data_.size() + delta);
  return true;
}

void CborEncodeBuffer::SetType(Type type) {
  data_[position_ - 1] = static_cast<uint8_t>(type);
}

bool IsError(ssize_t x) {
  return x < 0;
}
)";
  dprintf(fd, prologue, header_filename.c_str());
  return true;
}

bool WriteSourceEpilogue(int fd) {
  static const char epilogue[] = R"(
}  // namespace msgs
}  // namespace openscreen)";
  dprintf(fd, epilogue);
  return true;
}