//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Common hash functions with convenient interfaces.

#pragma once
#include <stddef.h>
#include <stdint.h>

#include "rocksdb/slice.h"
#include "util/xxhash.h"

namespace rocksdb {

// Non-persistent hash. Must only used for in-memory data structure.
// The hash results are thus applicable to change. (Thus, it rarely makes
// sense to specify a seed for this function.)
inline uint64_t NPHash64(const char* data, size_t n, uint32_t seed = 0) {
  // XXH3 currently experimental, but generally faster than other quality
  // 64-bit hash functions.
  return XXH3_64bits_withSeed(data, n, seed);
}

extern uint32_t Hash(const char* data, size_t n, uint32_t seed);

inline uint32_t BloomHash(const Slice& key) {
  return Hash(key.data(), key.size(), 0xbc9f1d34);
}

inline uint64_t GetSliceNPHash64(const Slice& s) {
  return NPHash64(s.data(), s.size());
}

inline uint32_t GetSliceHash(const Slice& s) {
  return Hash(s.data(), s.size(), 397);
}

// std::hash compatible interface.
struct SliceHasher {
  uint32_t operator()(const Slice& s) const { return GetSliceHash(s); }
};

// An alternative to % for mapping a hash value to an arbitrary range. See
// https://github.com/lemire/fastrange
inline uint32_t fastrange32(uint32_t hash, uint32_t range) {
  uint64_t product = uint64_t{range} * hash;
  return static_cast<uint32_t>(product >> 32);
}

// An alternative to % for mapping a 64-bit hash value to an arbitrary range
// that fits in size_t. See https://github.com/lemire/fastrange
// We find size_t more convenient than uint64_t for the range, with side
// benefit of better optimization on 32-bit platforms.
inline size_t fastrange64(uint64_t hash, size_t range) {
#if defined(HAVE_UINT128_EXTENSION)
  // Can use compiler's 128-bit type. Trust it to do the right thing.
  __uint128_t wide = __uint128_t{range} * hash;
  return static_cast<size_t>(wide >> 64);
#else
  // Fall back: full decomposition.
  // NOTE: GCC seems to fully understand this code as 64-bit x {32 or 64}-bit
  // -> {96 or 128}-bit multiplication and optimize it down to a single
  // wide-result multiplication (64-bit platform) or two wide-result
  // multiplications (32-bit platforms, where range64 >> 32 is zero).
  uint64_t range64 = range;  // ok to shift by 32, even if size_t is 32-bit
  uint64_t tmp = uint64_t{range64 & 0xffffFFFF} * uint64_t{hash & 0xffffFFFF};
  tmp >>= 32;
  tmp += uint64_t{range64 & 0xffffFFFF} * uint64_t{hash >> 32};
  // Avoid overflow: first add lower 32 of tmp2, and later upper 32
  uint64_t tmp2 = uint64_t{range64 >> 32} * uint64_t{hash & 0xffffFFFF};
  tmp += static_cast<uint32_t>(tmp2);
  tmp >>= 32;
  tmp += (tmp2 >> 32);
  tmp += uint64_t{range64 >> 32} * uint64_t{hash >> 32};
  return static_cast<size_t>(tmp);
#endif
}

}  // namespace rocksdb
