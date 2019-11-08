// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// A database can be configured with a custom FilterPolicy object.
// This object is responsible for creating a small filter from a set
// of keys.  These filters are stored in rocksdb and are consulted
// automatically by rocksdb to decide whether or not to read some
// information from disk. In many cases, a filter can cut down the
// number of disk seeks form a handful to a single disk seek per
// DB::Get() call.
//
// Most people will want to use the builtin bloom filter support (see
// NewBloomFilterPolicy() below).

#pragma once

#include <stdlib.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace rocksdb {

class Slice;

// A class that takes a bunch of keys, then generates filter
class FilterBitsBuilder {
 public:
  virtual ~FilterBitsBuilder() {}

  // Add Key to filter, you could use any way to store the key.
  // Such as: storing hashes or original keys
  // Keys are in sorted order and duplicated keys are possible.
  virtual void AddKey(const Slice& key) = 0;

  // Generate the filter using the keys that are added
  // The return value of this function would be the filter bits,
  // The ownership of actual data is set to buf
  virtual Slice Finish(std::unique_ptr<const char[]>* buf) = 0;

  // Calculate num of keys that can be added and generate a filter
  // <= the specified number of bytes.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4702)  // unreachable code
#endif
  virtual int CalculateNumEntry(const uint32_t /*bytes*/) {
#ifndef ROCKSDB_LITE
    throw std::runtime_error("CalculateNumEntry not Implemented");
#else
    abort();
#endif
    return 0;
  }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
};

// A class that checks if a key can be in filter
// It should be initialized by Slice generated by BitsBuilder
class FilterBitsReader {
 public:
  virtual ~FilterBitsReader() {}

  // Check if the entry match the bits in filter
  virtual bool MayMatch(const Slice& entry) = 0;

  // Check if an array of entries match the bits in filter
  virtual void MayMatch(int num_keys, Slice** keys, bool* may_match) {
    for (int i = 0; i < num_keys; ++i) {
      may_match[i] = MayMatch(*keys[i]);
    }
  }
};

// We add a new format of filter block called full filter block
// This new interface gives you more space of customization
//
// For the full filter block, you can plug in your version by implement
// the FilterBitsBuilder and FilterBitsReader
//
// There are two sets of interface in FilterPolicy
// Set 1: CreateFilter, KeyMayMatch: used for blockbased filter
// Set 2: GetFilterBitsBuilder, GetFilterBitsReader, they are used for
// full filter.
// Set 1 MUST be implemented correctly, Set 2 is optional
// RocksDB would first try using functions in Set 2. if they return nullptr,
// it would use Set 1 instead.
// You can choose filter type in NewBloomFilterPolicy
class FilterPolicy {
 public:
  virtual ~FilterPolicy();

  // Return the name of this policy.  Note that if the filter encoding
  // changes in an incompatible way, the name returned by this method
  // must be changed.  Otherwise, old incompatible filters may be
  // passed to methods of this type.
  virtual const char* Name() const = 0;

  // keys[0,n-1] contains a list of keys (potentially with duplicates)
  // that are ordered according to the user supplied comparator.
  // Append a filter that summarizes keys[0,n-1] to *dst.
  //
  // Warning: do not change the initial contents of *dst.  Instead,
  // append the newly constructed filter to *dst.
  virtual void CreateFilter(const Slice* keys, int n,
                            std::string* dst) const = 0;

  // "filter" contains the data appended by a preceding call to
  // CreateFilter() on this class.  This method must return true if
  // the key was in the list of keys passed to CreateFilter().
  // This method may return true or false if the key was not on the
  // list, but it should aim to return false with a high probability.
  virtual bool KeyMayMatch(const Slice& key, const Slice& filter) const = 0;

  // Return a new FilterBitsBuilder for full or partitioned filter blocks, or
  // nullptr if using block-based filter.
  virtual FilterBitsBuilder* GetFilterBitsBuilder() const { return nullptr; }

  // Return a new FilterBitsReader for full or partitioned filter blocks, or
  // nullptr if using block-based filter.
  // As here, the input slice should NOT be deleted by FilterPolicy.
  virtual FilterBitsReader* GetFilterBitsReader(
      const Slice& /*contents*/) const {
    return nullptr;
  }
};

// Return a new filter policy that uses a bloom filter with approximately
// the specified number of bits per key.
//
// bits_per_key: bits per key in bloom filter. A good value for bits_per_key
// is 10, which yields a filter with ~ 1% false positive rate.
// use_block_based_builder: use deprecated block based filter (true) rather
// than full or partitioned filter (false).
//
// Callers must delete the result after any database that is using the
// result has been closed.
//
// Note: if you are using a custom comparator that ignores some parts
// of the keys being compared, you must not use NewBloomFilterPolicy()
// and must provide your own FilterPolicy that also ignores the
// corresponding parts of the keys.  For example, if the comparator
// ignores trailing spaces, it would be incorrect to use a
// FilterPolicy (like NewBloomFilterPolicy) that does not ignore
// trailing spaces in keys.
extern const FilterPolicy* NewBloomFilterPolicy(
    int bits_per_key, bool use_block_based_builder = false);
}  // namespace rocksdb
