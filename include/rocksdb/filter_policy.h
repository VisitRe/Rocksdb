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

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "rocksdb/advanced_options.h"
#include "rocksdb/memory_allocator.h"
#include "rocksdb/status.h"
#include "rocksdb/types.h"

namespace ROCKSDB_NAMESPACE {

class Slice;
struct BlockBasedTableOptions;
struct ConfigOptions;

// A class that takes a bunch of keys, then generates filter
class FilterBitsBuilder {
 public:
  virtual ~FilterBitsBuilder() {}

  // Add a key (or prefix) to the filter. Typically, a builder will keep
  // a set of 64-bit key hashes and only build the filter in Finish
  // when the final number of keys is known. Keys are added in sorted order
  // and duplicated keys are possible, so typically, the builder will
  // only add this key if its hash is different from the most recently
  // added.
  virtual void AddKey(const Slice& key) = 0;

  // Called by RocksDB before Finish to populate
  // TableProperties::num_filter_entries, so should represent the
  // number of unique keys (and/or prefixes) added, but does not have
  // to be exact. `return 0;` may be used to conspicuously indicate "unknown".
  virtual size_t EstimateEntriesAdded() = 0;

  // Generate the filter using the keys that are added
  // The return value of this function would be the filter bits,
  // The ownership of actual data is set to buf.
  // RocksDB does not call this function in production except via the default
  // implementation of FinishV2. The default implementation of this function
  // calls FinishV2, so one of Finish and FinishV2 must be overridden (to
  // avoid call stack overflow).
  virtual Slice Finish(std::unique_ptr<const char[]>* buf);

  // Newer version of Finish() that supports
  // * Returning a Status, in case an unexpected memory corruption is detected
  // in filter construction.
  // * Allocating the returned buffer with a custom allocator (such as for
  // cache warming during table construction)
  //
  // If returning Status::OK() and output_filter->size() > 0, then
  // output_filter->data() points to a buffer allocated by `allocator` (or
  // default allocator with allocator==nullptr). output_filter->size() might
  // be larger than the original requested allocation size if
  // allocator->UsableSize() says the space is usable. (See
  // optimize_filters_for_memory.)
  //
  // If not returning Status::OK(), then `output_filter` should be unmodified
  // or reset to Slice(). (Caller is not responsible for any deallocation.)
  //
  // The default implementation uses Finish() in a backwards-compatible way,
  // copying the buffer if needed to use the specified allocator.
  virtual Status FinishV2(MemoryAllocator* allocator, Slice* output_filter);

  // Verify the filter returned from calling FilterBitsBuilder::Finish.
  // The function returns Status::Corruption() if there is any corruption in the
  // constructed filter or Status::OK() otherwise.
  //
  // Implementations should normally consult
  // FilterBuildingContext::table_options.detect_filter_construct_corruption
  // to determine whether to perform verification or to skip by returning
  // Status::OK(). The decision is left to the FilterBitsBuilder so that
  // verification prerequisites before PostVerify can be skipped when not
  // configured.
  //
  // RocksDB internal will always call MaybePostVerify() on the filter after
  // it is returned from calling FilterBitsBuilder::Finish
  // except for FilterBitsBuilder::Finish resulting a corruption
  // status, which indicates the filter is already in a corrupted state and
  // there is no need to post-verify
  virtual Status MaybePostVerify(const Slice& /* filter_content */) {
    return Status::OK();
  }

  // Approximate the number of keys that can be added and generate a filter
  // <= the specified number of bytes. Callers (including RocksDB) should
  // only use this result for optimizing performance and not as a guarantee.
  virtual size_t ApproximateNumEntries(size_t bytes) = 0;
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

// Contextual information passed to BloomFilterPolicy at filter building time.
// Used in overriding FilterPolicy::GetBuilderWithContext(). References other
// structs because this is expected to be a temporary, stack-allocated object.
struct FilterBuildingContext {
  // This constructor is for internal use only and subject to change.
  FilterBuildingContext(const BlockBasedTableOptions& table_options);

  // Options for the table being built
  const BlockBasedTableOptions& table_options;

  // BEGIN from (DB|ColumnFamily)Options in effect at table creation time
  CompactionStyle compaction_style = kCompactionStyleLevel;

  // Number of LSM levels, or -1 if unknown
  int num_levels = -1;

  // An optional logger for reporting errors, warnings, etc.
  Logger* info_log = nullptr;
  // END from (DB|ColumnFamily)Options

  // Name of the column family for the table (or empty string if unknown)
  // TODO: consider changing to Slice
  std::string column_family_name;

  // The table level at time of constructing the SST file, or -1 if unknown
  // or N/A as in SstFileWriter. (The table file could later be used at a
  // different level.)
  int level_at_creation = -1;

  // True if known to be going into bottommost sorted run for applicable
  // key range (which might not even be last level with data). False
  // otherwise.
  bool is_bottommost = false;

  // Reason for creating the file with the filter
  TableFileCreationReason reason = TableFileCreationReason::kMisc;
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

  // Creates a new FilterPolicy based on the input value string and returns the
  // result The value might be an ID, and ID with properties, or an old-style
  // policy string.
  // The value describes the FilterPolicy being created.
  // For BloomFilters, value may be a ":"-delimited value of the form:
  //   "bloomfilter:[bits_per_key]:[use_block_based_builder]",
  //   e.g. ""bloomfilter:4:true"
  //   The above string is equivalent to calling NewBloomFilterPolicy(4, true).
  static Status CreateFromString(const ConfigOptions& config_options,
                                 const std::string& value,
                                 std::shared_ptr<const FilterPolicy>* result);

  // Return the name of this policy.  Note that if the filter encoding
  // changes in an incompatible way, the name returned by this method
  // must be changed.  Otherwise, old incompatible filters may be
  // passed to methods of this type.
  virtual const char* Name() const = 0;

  // Return a new FilterBitsBuilder for constructing full or partitioned
  // filter blocks. The configuration details can depend on the input
  // FilterBuildingContext but must be serialized such that FilterBitsReader
  // can operate based on the block contents without knowing a
  // FilterBuildingContext.
  // Change in 7.0 release: returning nullptr indicates "no filter"
  virtual FilterBitsBuilder* GetBuilderWithContext(
      const FilterBuildingContext&) const = 0;

  // Return a new FilterBitsReader for full or partitioned filter blocks.
  // Caller retains ownership of any buffer pointed to by the input Slice.
  virtual FilterBitsReader* GetFilterBitsReader(
      const Slice& /*contents*/) const {
    return nullptr;
  }
};

// Return a new filter policy that uses a bloom filter with approximately
// the specified number of bits per key. See
// https://github.com/facebook/rocksdb/wiki/RocksDB-Bloom-Filter
//
// bits_per_key: average bits allocated per key in bloom filter. A good
// choice is 9.9, which yields a filter with ~ 1% false positive rate.
// When format_version < 5, the value will be rounded to the nearest
// integer. Recommend using no more than three decimal digits after the
// decimal point, as in 6.667.
//
// To avoid configurations that are unlikely to produce good filtering
// value for the CPU overhead, bits_per_key < 0.5 is rounded down to 0.0
// which means "generate no filter", and 0.5 <= bits_per_key < 1.0 is
// rounded up to 1.0, for a 62% FP rate.
//
// The caller is responsible for eventually deleting the result, though
// this is typically handled automatically with BlockBasedTableOptions:
//   table_options.filter_policy.reset(NewBloomFilterPolicy(...));
//
// As of RocksDB 7.0, the use_block_based_builder parameter is ignored.
// (The old, inefficient block-based filter is no longer accessible in
// the public API.)
//
// Note: if you are using a custom comparator that ignores some parts
// of the keys being compared, you must not use NewBloomFilterPolicy()
// and must provide your own FilterPolicy that also ignores the
// corresponding parts of the keys.  For example, if the comparator
// ignores trailing spaces, it would be incorrect to use a
// FilterPolicy (like NewBloomFilterPolicy) that does not ignore
// trailing spaces in keys.
extern const FilterPolicy* NewBloomFilterPolicy(
    double bits_per_key, bool IGNORED_use_block_based_builder = false);

// A new Bloom alternative that saves about 30% space compared to
// Bloom filters, with similar query times but roughly 3-4x CPU time
// and 3x temporary space usage during construction.  For example, if
// you pass in 10 for bloom_equivalent_bits_per_key, you'll get the same
// 0.95% FP rate as Bloom filter but only using about 7 bits per key.
//
// The space savings of Ribbon filters makes sense for lower (higher
// numbered; larger; longer-lived) levels of LSM, whereas the speed of
// Bloom filters make sense for highest levels of LSM. Setting
// bloom_before_level allows for this design with Level and Universal
// compaction styles. For example, bloom_before_level=1 means that Bloom
// filters will be used in level 0, including flushes, and Ribbon
// filters elsewhere, including FIFO compaction and external SST files.
// For this option, memtable flushes are considered level -1 (so that
// flushes can be distinguished from intra-L0 compaction).
// bloom_before_level=0 (default) -> Generate Bloom filters only for
// flushes under Level and Universal compaction styles.
// bloom_before_level=-1 -> Always generate Ribbon filters (except in
// some extreme or exceptional cases).
//
// Ribbon filters are compatible with RocksDB >= 6.15.0. Earlier
// versions reading the data will behave as if no filter was used
// (degraded performance until compaction rebuilds filters). All
// built-in FilterPolicies (Bloom or Ribbon) are able to read other
// kinds of built-in filters.
//
// Note: the current Ribbon filter schema uses some extra resources
// when constructing very large filters. For example, for 100 million
// keys in a single filter (one SST file without partitioned filters),
// 3GB of temporary, untracked memory is used, vs. 1GB for Bloom.
// However, the savings in filter space from just ~60 open SST files
// makes up for the additional temporary memory use.
//
// Also consider using optimize_filters_for_memory to save filter
// memory.
extern const FilterPolicy* NewRibbonFilterPolicy(
    double bloom_equivalent_bits_per_key, int bloom_before_level = 0);

}  // namespace ROCKSDB_NAMESPACE
