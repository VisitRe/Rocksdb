//  Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <assert.h>
#include <climits>
#ifdef _MSC_VER
#include <intrin.h>
#endif

#include <cstdint>
#include <type_traits>

#include "rocksdb/rocksdb_namespace.h"

namespace ROCKSDB_NAMESPACE {

// Fast implementation of floor(log2(v)). Undefined for 0 or negative
// numbers (in case of signed type).
template <typename T>
inline int FloorLog2(T v) {
  static_assert(std::is_integral<T>::value, "non-integral type");
  assert(v > 0);
#ifdef _MSC_VER
  static_assert(sizeof(T) <= sizeof(uint64_t), "type too big");
  unsigned long idx = 0;
  if (sizeof(T) <= sizeof(uint32_t)) {
    _BitScanReverse(&idx, static_cast<uint32_t>(v));
  } else {
#if defined(_M_X64) || defined(_M_ARM64)
    _BitScanReverse64(&idx, static_cast<uint64_t>(v));
#else
    const auto vh = static_cast<uint32_t>(static_cast<uint64_t>(v) >> 32);
    if (vh != 0) {
      _BitScanReverse(&idx, static_cast<uint32_t>(vh));
      idx += 32;
    } else {
      _BitScanReverse(&idx, static_cast<uint32_t>(v));
    }
#endif
  }
  return idx;
#else
  static_assert(sizeof(T) <= sizeof(unsigned long long), "type too big");
  if (sizeof(T) <= sizeof(unsigned int)) {
    int lz = __builtin_clz(static_cast<unsigned int>(v));
    return int{sizeof(unsigned int)} * 8 - 1 - lz;
  } else if (sizeof(T) <= sizeof(unsigned long)) {
    int lz = __builtin_clzl(static_cast<unsigned long>(v));
    return int{sizeof(unsigned long)} * 8 - 1 - lz;
  } else {
    int lz = __builtin_clzll(static_cast<unsigned long long>(v));
    return int{sizeof(unsigned long long)} * 8 - 1 - lz;
  }
#endif
}

// Constexpr version of FloorLog2
template <typename T>
constexpr int ConstexprFloorLog2(T v) {
  int rv = 0;
  while (v > T{1}) {
    ++rv;
    v >>= 1;
  }
  return rv;
}

// Number of low-order zero bits before the first 1 bit. Undefined for 0.
template <typename T>
inline int CountTrailingZeroBits(T v) {
  static_assert(std::is_integral<T>::value, "non-integral type");
  assert(v != 0);
#ifdef _MSC_VER
  static_assert(sizeof(T) <= sizeof(uint64_t), "type too big");
  unsigned long tz = 0;
  if (sizeof(T) <= sizeof(uint32_t)) {
    _BitScanForward(&tz, static_cast<uint32_t>(v));
  } else {
#if defined(_M_X64) || defined(_M_ARM64)
    _BitScanForward64(&tz, static_cast<uint64_t>(v));
#else
    _BitScanForward(&tz, static_cast<uint32_t>(v));
    if (tz == 0) {
      _BitScanForward(&tz,
                      static_cast<uint32_t>(static_cast<uint64_t>(v) >> 32));
      tz += 32;
    }
#endif
  }
  return static_cast<int>(tz);
#else
  static_assert(sizeof(T) <= sizeof(unsigned long long), "type too big");
  if (sizeof(T) <= sizeof(unsigned int)) {
    return __builtin_ctz(static_cast<unsigned int>(v));
  } else if (sizeof(T) <= sizeof(unsigned long)) {
    return __builtin_ctzl(static_cast<unsigned long>(v));
  } else {
    return __builtin_ctzll(static_cast<unsigned long long>(v));
  }
#endif
}

// Not all MSVC compile settings will use `BitsSetToOneFallback()`. We include
// the following code at coarse granularity for simpler macros. It's important
// to exclude at least so our non-MSVC unit test coverage tool doesn't see it.
#ifdef _MSC_VER

namespace detail {

template <typename T>
int BitsSetToOneFallback(T v) {
  const int kBits = static_cast<int>(sizeof(T)) * 8;
  static_assert((kBits & (kBits - 1)) == 0, "must be power of two bits");
  // we static_cast these bit patterns in order to truncate them to the correct
  // size. Warning C4309 dislikes this technique, so disable it here.
#pragma warning(disable : 4309)
  v = static_cast<T>(v - ((v >> 1) & static_cast<T>(0x5555555555555555ull)));
  v = static_cast<T>((v & static_cast<T>(0x3333333333333333ull)) +
                     ((v >> 2) & static_cast<T>(0x3333333333333333ull)));
  v = static_cast<T>((v + (v >> 4)) & static_cast<T>(0x0F0F0F0F0F0F0F0Full));
#pragma warning(default : 4309)
  for (int shift_bits = 8; shift_bits < kBits; shift_bits <<= 1) {
    v += static_cast<T>(v >> shift_bits);
  }
  // we want the bottom "slot" that's big enough to represent a value up to
  // (and including) kBits.
  return static_cast<int>(v & static_cast<T>(kBits | (kBits - 1)));
}

}  // namespace detail

#endif  // _MSC_VER

// Number of bits set to 1. Also known as "population count".
template <typename T>
inline int BitsSetToOne(T v) {
  static_assert(std::is_integral<T>::value, "non-integral type");
#ifdef _MSC_VER
  static_assert(sizeof(T) <= sizeof(uint64_t), "type too big");
  if (sizeof(T) < sizeof(uint32_t)) {
    // This bit mask is to avoid a compiler warning on unused path
    constexpr auto mm = 8 * sizeof(uint32_t) - 1;
    // The bit mask is to neutralize sign extension on small signed types
    constexpr uint32_t m = (uint32_t{1} << ((8 * sizeof(T)) & mm)) - 1;
#if defined(HAVE_SSE42) && (defined(_M_X64) || defined(_M_IX86))
    return static_cast<int>(__popcnt(static_cast<uint32_t>(v) & m));
#else
    return static_cast<int>(detail::BitsSetToOneFallback(v) & m);
#endif
  } else if (sizeof(T) == sizeof(uint32_t)) {
#if defined(HAVE_SSE42) && (defined(_M_X64) || defined(_M_IX86))
    return static_cast<int>(__popcnt(static_cast<uint32_t>(v)));
#else
    return detail::BitsSetToOneFallback(static_cast<uint32_t>(v));
#endif
  } else {
#if defined(HAVE_SSE42) && defined(_M_X64)
    return static_cast<int>(__popcnt64(static_cast<uint64_t>(v)));
#elif defined(HAVE_SSE42) && defined(_M_IX86)
    return static_cast<int>(
        __popcnt(static_cast<uint32_t>(static_cast<uint64_t>(v) >> 32) +
                 __popcnt(static_cast<uint32_t>(v))));
#else
    return detail::BitsSetToOneFallback(static_cast<uint64_t>(v));
#endif
  }
#else
  static_assert(sizeof(T) <= sizeof(unsigned long long), "type too big");
  if (sizeof(T) < sizeof(unsigned int)) {
    // This bit mask is to avoid a compiler warning on unused path
    constexpr auto mm = 8 * sizeof(unsigned int) - 1;
    // This bit mask is to neutralize sign extension on small signed types
    constexpr unsigned int m = (1U << ((8 * sizeof(T)) & mm)) - 1;
    return __builtin_popcount(static_cast<unsigned int>(v) & m);
  } else if (sizeof(T) == sizeof(unsigned int)) {
    return __builtin_popcount(static_cast<unsigned int>(v));
  } else if (sizeof(T) <= sizeof(unsigned long)) {
    return __builtin_popcountl(static_cast<unsigned long>(v));
  } else {
    return __builtin_popcountll(static_cast<unsigned long long>(v));
  }
#endif
}

template <typename T>
inline int BitParity(T v) {
  static_assert(std::is_integral<T>::value, "non-integral type");
#ifdef _MSC_VER
  // bit parity == oddness of popcount
  return BitsSetToOne(v) & 1;
#else
  static_assert(sizeof(T) <= sizeof(unsigned long long), "type too big");
  if (sizeof(T) <= sizeof(unsigned int)) {
    // On any sane systen, potential sign extension here won't change parity
    return __builtin_parity(static_cast<unsigned int>(v));
  } else if (sizeof(T) <= sizeof(unsigned long)) {
    return __builtin_parityl(static_cast<unsigned long>(v));
  } else {
    return __builtin_parityll(static_cast<unsigned long long>(v));
  }
#endif
}

// Swaps between big and little endian. Can be used in combination with the
// little-endian encoding/decoding functions in coding_lean.h and coding.h to
// encode/decode big endian.
template <typename T>
inline T EndianSwapValue(T v) {
  static_assert(std::is_integral<T>::value, "non-integral type");

#ifdef _MSC_VER
  if (sizeof(T) == 2) {
    return static_cast<T>(_byteswap_ushort(static_cast<uint16_t>(v)));
  } else if (sizeof(T) == 4) {
    return static_cast<T>(_byteswap_ulong(static_cast<uint32_t>(v)));
  } else if (sizeof(T) == 8) {
    return static_cast<T>(_byteswap_uint64(static_cast<uint64_t>(v)));
  }
#else
  if (sizeof(T) == 2) {
    return static_cast<T>(__builtin_bswap16(static_cast<uint16_t>(v)));
  } else if (sizeof(T) == 4) {
    return static_cast<T>(__builtin_bswap32(static_cast<uint32_t>(v)));
  } else if (sizeof(T) == 8) {
    return static_cast<T>(__builtin_bswap64(static_cast<uint64_t>(v)));
  }
#endif
  // Recognized by clang as bswap, but not by gcc :(
  T ret_val = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    ret_val |= ((v >> (8 * i)) & 0xff) << (8 * (sizeof(T) - 1 - i));
  }
  return ret_val;
}

// Reverses the order of bits in an integral value
template <typename T>
inline T ReverseBits(T v) {
  T r = EndianSwapValue(v);
  const T kHighestByte = T{1} << ((sizeof(T) - 1) * 8);
  const T kEveryByte = kHighestByte | (kHighestByte / 255);

  r = ((r & (kEveryByte * 0x0f)) << 4) | ((r >> 4) & (kEveryByte * 0x0f));
  r = ((r & (kEveryByte * 0x33)) << 2) | ((r >> 2) & (kEveryByte * 0x33));
  r = ((r & (kEveryByte * 0x55)) << 1) | ((r >> 1) & (kEveryByte * 0x55));

  return r;
}

// Returns x % 2^b. Assumes the size of x's data type is
// at least b bits.
template<typename T>
inline T BinaryMod(T x, uint8_t b) {
  std::size_t size = sizeof(T) << 3;
  assert(b <= size);
  return (x << (size - b)) >> (size - b);
}

}  // namespace ROCKSDB_NAMESPACE
