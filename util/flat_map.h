// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UTIL_FLAT_MAP_H_
#define UTIL_FLAT_MAP_H_

#include <algorithm>
#include <initializer_list>
#include <utility>
#include <vector>

namespace openscreen {

// For small numbers of elements, a vector is much more efficient than a
// map or unordered_map due to not needing hashing. FlatMap allows for
// using map-like syntax but is backed by a std::vector, combining all the
// performance of a vector with the convenience of a map.
//
// NOTE: this class allows usage of const char* as Key or Value types, but
// it is generally recommended that you use std::string, or std::string_view
// for literals. string_view is similarly efficient to a raw char* pointer,
// but gives sizing and equality operators, among other features.
template <class Key, class Value>
class FlatMap final : public std::vector<std::pair<Key, Value>> {
 public:
  using iterator = typename std::vector<std::pair<Key, Value>>::iterator;
  using const_iterator =
      typename std::vector<std::pair<Key, Value>>::const_iterator;

  using std::vector<std::pair<Key, Value>>::vector;
  FlatMap(std::initializer_list<std::pair<Key, Value>> init_list)
      : std::vector<std::pair<Key, Value>>(init_list) {}
  FlatMap() = default;
  FlatMap(const FlatMap&) = default;
  FlatMap(FlatMap&&) noexcept = default;
  FlatMap& operator=(const FlatMap&) = default;
  FlatMap& operator=(FlatMap&&) = default;
  ~FlatMap() = default;

  // Accessors that wrap std::find_if, and return an iterator to the key value
  // pair.
  iterator find(const Key& key) {
    return std::find_if(this->begin(), this->end(),
                        [&key](const std::pair<Key, Value>& pair) {
                          return key == pair.first;
                        });
  }

  const_iterator find(const Key& key) const {
    return std::find_if(this->begin(), this->end(),
                        [&key](const std::pair<Key, Value>& pair) {
                          return key == pair.first;
                        });
  }

  // Returns true if the map contains an entry with key `key`.
  bool contains(const Key& key) const { return find(key) != this->end(); }

  // Remove an entry from the map. Returns an iterator pointing to the new
  // location of the element that followed the last element erased by the
  // function call. This is the container end if the operation erased the last
  // element in the sequence.
  iterator erase_key(const Key& key) {
    auto it = find(key);
    if (it == this->end()) {
      return this->end();
    }
    return this->erase(it);
  }
};

}  // namespace openscreen

#endif  // UTIL_FLAT_MAP_H_
