/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef INCLUDE_PERFETTO_EXT_BASE_HASH_H_
#define INCLUDE_PERFETTO_EXT_BASE_HASH_H_

#include <stddef.h>
#include <stdint.h>
#include <type_traits>

namespace perfetto {
namespace base {

// A helper class which computes a 64-bit hash of the input data.
// The algorithm used is FNV-1a as it is fast and easy to implement and has
// relatively few collisions.
// WARNING: This hash function should not be used for any cryptographic purpose.
class Hash {
 public:
  // Creates an empty hash object
  Hash() {}

  // Hashes a numeric value.
  template <
      typename T,
      typename std::enable_if<std::is_arithmetic<T>::value, bool>::type = true>
  void Update(T data) {
    Update(reinterpret_cast<const char*>(&data), sizeof(data));
  }

  // Hashes a byte array.
  void Update(const char* data, size_t size) {
    for (size_t i = 0; i < size; i++) {
      result_ ^= static_cast<uint8_t>(data[i]);
      // Note: Arithmetic overflow of unsigned integers is well defined in C++
      // standard unlike signed integers.
      // https://stackoverflow.com/a/41280273
      result_ *= kFnv1a64Prime;
    }
  }

  uint64_t digest() const { return result_; }

 private:
  static constexpr uint64_t kFnv1a64OffsetBasis = 0xcbf29ce484222325;
  static constexpr uint64_t kFnv1a64Prime = 0x100000001b3;

  uint64_t result_ = kFnv1a64OffsetBasis;
};

// This is for using already-hashed key into std::unordered_map and avoid the
// cost of re-hashing. Example:
// unordered_map<uint64_t, Value, AlreadyHashed> my_map.
template <typename T>
struct AlreadyHashed {
  size_t operator()(const T& x) const { return static_cast<size_t>(x); }
};

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_HASH_H_
