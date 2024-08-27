// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "rocksdb/filter_policy.h"
#include "rocksdb/table.h"

namespace ROCKSDB_NAMESPACE {

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

  // Add two entries to the filter, typically a key and, as the alternate,
  // its prefix. This differs from AddKey(key); AddKey(alt); in that there
  // is extra state for de-duplicating successive `alt` entries, as well
  // as successive `key` entries. And there is de-duplication between `key`
  // and `alt` entries, even in adjacent calls, because a whole key might
  // be its own prefix. More specifically,
  //  AddKey(k1);
  //  AddKeyAndAlt(k2, a2);  // de-dup k2<>k1, k2<>a2, a2<>k1
  //  AddKeyAndAlt(k3, a3);  // de-dup k3<>k2, a3<>a2, k3<>a2, a3<>k2
  //  AddKey(k4);            // de-dup k4<>k3 BUT NOT k4<>a3
  virtual void AddKeyAndAlt(const Slice& key, const Slice& alt) = 0;

  // Called by RocksDB before Finish to populate
  // TableProperties::num_filter_entries, so should represent the
  // number of unique keys (and/or prefixes) added. MUST return 0
  // if and only if none have been added, but otherwise can be estimated.
  virtual size_t EstimateEntriesAdded() = 0;

  // Generate the filter using the keys that are added
  // The return value of this function would be the filter bits,
  // The ownership of actual data is set to buf
  virtual Slice Finish(std::unique_ptr<const char[]>* buf) = 0;

  // Similar to Finish(std::unique_ptr<const char[]>* buf), except that
  // for a non-null status pointer argument, it will point to
  // Status::Corruption() when there is any corruption during filter
  // construction or Status::OK() otherwise.
  //
  // WARNING: do not use a filter resulted from a corrupted construction
  // TODO: refactor this to have a better signature, consolidate
  virtual Slice Finish(std::unique_ptr<const char[]>* buf,
                       Status* /* status */) {
    return Finish(buf);
  }

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

// Exposes any extra information needed for testing built-in
// FilterBitsBuilders
class BuiltinFilterBitsBuilder : public FilterBitsBuilder {
 public:
  // Calculate number of bytes needed for a new filter, including
  // metadata. Passing the result to ApproximateNumEntries should
  // (ideally, usually) return >= the num_entry passed in.
  // When optimize_filters_for_memory is enabled, this function
  // is not authoritative but represents a target size that should
  // be close to the average size.
  virtual size_t CalculateSpace(size_t num_entries) = 0;

  // Returns an estimate of the FP rate of the returned filter if
  // `num_entries` keys are added and the filter returned by Finish
  // is `bytes` bytes.
  virtual double EstimatedFpRate(size_t num_entries, size_t bytes) = 0;
};

// Base class for RocksDB built-in filter reader with
// extra useful functionalities for inernal.
class BuiltinFilterBitsReader : public FilterBitsReader {
 public:
  // Check if the hash of the entry match the bits in filter
  virtual bool HashMayMatch(const uint64_t /* h */) { return true; }
};

// Base class for RocksDB built-in filter policies. This provides the
// ability to read all kinds of built-in filters (so that old filters can
// be used even when you change between built-in policies).
class BuiltinFilterPolicy : public FilterPolicy {
 public:  // overrides
  // Read metadata to determine what kind of FilterBitsReader is needed
  // and return a new one. This must successfully process any filter data
  // generated by a built-in FilterBitsBuilder, regardless of the impl
  // chosen for this BloomFilterPolicy.
  FilterBitsReader* GetFilterBitsReader(const Slice& contents) const override;
  static const char* kClassName();
  bool IsInstanceOf(const std::string& id) const override;
  // All variants of BuiltinFilterPolicy can read each others filters.
  const char* CompatibilityName() const override;
  static const char* kCompatibilityName();

 public:  // new
  // An internal function for the implementation of
  // BuiltinFilterBitsReader::GetFilterBitsReader without requiring an instance
  // or working around potential virtual overrides.
  static BuiltinFilterBitsReader* GetBuiltinFilterBitsReader(
      const Slice& contents);

  // Returns a new FilterBitsBuilder from the filter_policy in
  // table_options of a context, or nullptr if not applicable.
  // (An internal convenience function to save boilerplate.)
  static FilterBitsBuilder* GetBuilderFromContext(const FilterBuildingContext&);

 private:
  // For Bloom filter implementation(s)
  static BuiltinFilterBitsReader* GetBloomBitsReader(const Slice& contents);

  // For Ribbon filter implementation(s)
  static BuiltinFilterBitsReader* GetRibbonBitsReader(const Slice& contents);
};

// A "read only" filter policy used for backward compatibility with old
// OPTIONS files, which did not specifying a Bloom configuration, just
// "rocksdb.BuiltinBloomFilter". Although this can read existing filters,
// this policy does not build new filters, so new SST files generated
// under the policy will get no filters (like nullptr FilterPolicy).
// This class is considered internal API and subject to change.
class ReadOnlyBuiltinFilterPolicy : public BuiltinFilterPolicy {
 public:
  const char* Name() const override { return kClassName(); }
  static const char* kClassName();

  // Does not write filters.
  FilterBitsBuilder* GetBuilderWithContext(
      const FilterBuildingContext&) const override {
    return nullptr;
  }
};

// RocksDB built-in filter policy for Bloom or Bloom-like filters including
// Ribbon filters.
// This class is considered internal API and subject to change.
// See NewBloomFilterPolicy and NewRibbonFilterPolicy.
class BloomLikeFilterPolicy : public BuiltinFilterPolicy {
 public:
  explicit BloomLikeFilterPolicy(double bits_per_key);

  ~BloomLikeFilterPolicy() override;
  static const char* kClassName();
  bool IsInstanceOf(const std::string& id) const override;

  std::string GetId() const override;

  // Essentially for testing only: configured millibits/key
  int GetMillibitsPerKey() const { return millibits_per_key_; }
  // Essentially for testing only: legacy whole bits/key
  int GetWholeBitsPerKey() const { return whole_bits_per_key_; }

  // All the different underlying implementations that a BloomLikeFilterPolicy
  // might use, as a configuration string name for a testing mode for
  // "always use this implementation." Only appropriate for unit tests.
  static const std::vector<std::string>& GetAllFixedImpls();

  // Convenience function for creating by name for fixed impls
  static std::shared_ptr<const FilterPolicy> Create(const std::string& name,
                                                    double bits_per_key);

 protected:
  // Some implementations used by aggregating policies
  FilterBitsBuilder* GetLegacyBloomBuilderWithContext(
      const FilterBuildingContext& context) const;
  FilterBitsBuilder* GetFastLocalBloomBuilderWithContext(
      const FilterBuildingContext& context) const;
  FilterBitsBuilder* GetStandard128RibbonBuilderWithContext(
      const FilterBuildingContext& context) const;

  std::string GetBitsPerKeySuffix() const;

 private:
  // Bits per key settings are for configuring Bloom filters.

  // Newer filters support fractional bits per key. For predictable behavior
  // of 0.001-precision values across floating point implementations, we
  // round to thousandths of a bit (on average) per key.
  int millibits_per_key_;

  // Older filters round to whole number bits per key. (There *should* be no
  // compatibility issue with fractional bits per key, but preserving old
  // behavior with format_version < 5 just in case.)
  int whole_bits_per_key_;

  // For configuring Ribbon filter: a desired value for 1/fp_rate. For
  // example, 100 -> 1% fp rate.
  double desired_one_in_fp_rate_;

  // Whether relevant warnings have been logged already. (Remember so we
  // only report once per BloomFilterPolicy instance, to keep the noise down.)
  mutable std::atomic<bool> warned_;

  // State for implementing optimize_filters_for_memory. Essentially, this
  // tracks a surplus or deficit in total FP rate of filters generated by
  // builders under this policy vs. what would have been generated without
  // optimize_filters_for_memory.
  //
  // To avoid floating point weirdness, the actual value is
  //  Sum over all generated filters f:
  //   (predicted_fp_rate(f) - predicted_fp_rate(f|o_f_f_m=false)) * 2^32
  mutable std::atomic<int64_t> aggregate_rounding_balance_;
};

// For NewBloomFilterPolicy
//
// This is a user-facing policy that automatically choose between
// LegacyBloom and FastLocalBloom based on context at build time,
// including compatibility with format_version.
class BloomFilterPolicy : public BloomLikeFilterPolicy {
 public:
  explicit BloomFilterPolicy(double bits_per_key);

  // To use this function, call BuiltinFilterPolicy::GetBuilderFromContext().
  //
  // Neither the context nor any objects therein should be saved beyond
  // the call to this function, unless it's shared_ptr.
  FilterBitsBuilder* GetBuilderWithContext(
      const FilterBuildingContext&) const override;

  static const char* kClassName();
  const char* Name() const override { return kClassName(); }
  static const char* kNickName();
  const char* NickName() const override { return kNickName(); }
  std::string GetId() const override;
};

// For NewRibbonFilterPolicy
//
// This is a user-facing policy that chooses between Standard128Ribbon
// and FastLocalBloom based on context at build time (LSM level and other
// factors in extreme cases).
class RibbonFilterPolicy : public BloomLikeFilterPolicy {
 public:
  explicit RibbonFilterPolicy(double bloom_equivalent_bits_per_key,
                              int bloom_before_level);

  FilterBitsBuilder* GetBuilderWithContext(
      const FilterBuildingContext&) const override;

  int GetBloomBeforeLevel() const { return bloom_before_level_; }

  static const char* kClassName();
  const char* Name() const override { return kClassName(); }
  static const char* kNickName();
  const char* NickName() const override { return kNickName(); }
  static const char* kName();
  std::string GetId() const override;

 private:
  std::atomic<int> bloom_before_level_;
};

// For testing only, but always constructable with internal names
namespace test {

class LegacyBloomFilterPolicy : public BloomLikeFilterPolicy {
 public:
  explicit LegacyBloomFilterPolicy(double bits_per_key)
      : BloomLikeFilterPolicy(bits_per_key) {}

  FilterBitsBuilder* GetBuilderWithContext(
      const FilterBuildingContext& context) const override;

  static const char* kClassName();
  const char* Name() const override { return kClassName(); }
};

class FastLocalBloomFilterPolicy : public BloomLikeFilterPolicy {
 public:
  explicit FastLocalBloomFilterPolicy(double bits_per_key)
      : BloomLikeFilterPolicy(bits_per_key) {}

  FilterBitsBuilder* GetBuilderWithContext(
      const FilterBuildingContext& context) const override;

  static const char* kClassName();
  const char* Name() const override { return kClassName(); }
};

class Standard128RibbonFilterPolicy : public BloomLikeFilterPolicy {
 public:
  explicit Standard128RibbonFilterPolicy(double bloom_equiv_bits_per_key)
      : BloomLikeFilterPolicy(bloom_equiv_bits_per_key) {}

  FilterBitsBuilder* GetBuilderWithContext(
      const FilterBuildingContext& context) const override;

  static const char* kClassName();
  const char* Name() const override { return kClassName(); }
};

}  // namespace test

}  // namespace ROCKSDB_NAMESPACE
