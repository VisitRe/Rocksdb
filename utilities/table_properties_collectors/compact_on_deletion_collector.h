//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include "rocksdb/utilities/table_properties_collectors.h"
namespace ROCKSDB_NAMESPACE {

class CompactOnDeletionCollector : public TablePropertiesCollector {
 public:
  CompactOnDeletionCollector(size_t sliding_window_size,
                             size_t deletion_trigger, double deletion_raatio);

  // AddUserKey() will be called when a new key/value pair is inserted into the
  // table.
  // @params key    the user key that is inserted into the table.
  // @params value  the value that is inserted into the table.
  // @params file_size  file size up to now
  virtual Status AddUserKey(const Slice& key, const Slice& value,
                            EntryType type, SequenceNumber seq,
                            uint64_t file_size) override;

  // Finish() will be called when a table has already been built and is ready
  // for writing the properties block.
  // @params properties  User will add their collected statistics to
  // `properties`.
  virtual Status Finish(UserCollectedProperties* /*properties*/) override;

  // Return the human-readable properties, where the key is property name and
  // the value is the human-readable form of value.
  virtual UserCollectedProperties GetReadableProperties() const override {
    return UserCollectedProperties();
  }

  // The name of the properties collector can be used for debugging purpose.
  virtual const char* Name() const override {
    return "CompactOnDeletionCollector";
  }

  // EXPERIMENTAL Return whether the output file should be further compacted
  virtual bool NeedCompact() const override { return need_compaction_; }

  static const int kNumBuckets = 128;

 private:
  void Reset();

  // A ring buffer that used to count the number of deletion entries for every
  // "bucket_size_" keys.
  size_t num_deletions_in_buckets_[kNumBuckets];
  // the number of keys in a bucket
  size_t bucket_size_;

  size_t current_bucket_;
  size_t num_keys_in_current_bucket_;
  size_t num_deletions_in_observation_window_;
  size_t deletion_trigger_;
  const double deletion_ratio_;
  const bool deletion_ratio_enabled_;
  size_t total_entries_ = 0;
  size_t deletion_entries_ = 0;
  // true if the current SST file needs to be compacted.
  bool need_compaction_;
  bool finished_;
};
}  // namespace ROCKSDB_NAMESPACE
