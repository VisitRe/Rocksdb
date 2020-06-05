//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
#pragma once
#ifndef ROCKSDB_LITE

#include <unordered_set>

#include "db/blob/blob_index.h"
#include "monitoring/statistics.h"
#include "rocksdb/compaction_filter.h"
#include "rocksdb/env.h"
#include "rocksdb/utilities/layered_compaction_filter_base.h"
#include "utilities/blob_db/blob_db_gc_stats.h"
#include "utilities/blob_db/blob_db_impl.h"

namespace ROCKSDB_NAMESPACE {
namespace blob_db {

struct BlobCompactionContext {
  BlobDBImpl* blob_db_impl = nullptr;
  uint64_t next_file_number = 0;
  std::unordered_set<uint64_t> current_blob_files;
  SequenceNumber fifo_eviction_seq = 0;
  uint64_t evict_expiration_up_to = 0;
};

struct BlobCompactionContextGC {
  uint64_t cutoff_file_number = 0;
};

// Compaction filter that deletes expired blob indexes from the base DB.
// Comes into two varieties, one for the non-GC case and one for the GC case.
class BlobIndexCompactionFilterBase : public LayeredCompactionFilterBase {
 public:
  BlobIndexCompactionFilterBase(
      BlobCompactionContext&& blob_comp_context,
      const CompactionFilter* _user_comp_filter,
      std::unique_ptr<const CompactionFilter> _user_comp_filter_from_factory,
      uint64_t current_time, Statistics* stats)
      : LayeredCompactionFilterBase(_user_comp_filter,
                                    std::move(_user_comp_filter_from_factory)),
        context_(std::move(blob_comp_context)),
        current_time_(current_time),
        statistics_(stats) {}

  ~BlobIndexCompactionFilterBase() override;

  // Filter expired blob indexes regardless of snapshots.
  bool IgnoreSnapshots() const override { return true; }

  Decision FilterV2(int level, const Slice& key, ValueType value_type,
                    const Slice& value, std::string* new_value,
                    std::string* skip_until) const override;

 protected:
  bool IsBlobFileOpened() const;
  bool OpenNewBlobFileIfNeeded() const;
  bool ReadBlobFromOldFile(const Slice& key, const BlobIndex& blob_index,
                           PinnableSlice* blob, bool need_decompress,
                           CompressionType* compression_type) const;
  bool WriteBlobToNewFile(const Slice& key, const Slice& blob,
                          uint64_t* new_blob_file_number,
                          uint64_t* new_blob_offset) const;
  bool CloseAndRegisterNewBlobFileIfNeeded() const;
  bool CloseAndRegisterNewBlobFile() const;

  Statistics* statistics() const { return statistics_; }
  const BlobCompactionContext& context() const { return context_; }

 private:
  BlobCompactionContext context_;
  const uint64_t current_time_;
  Statistics* statistics_;

  mutable std::shared_ptr<BlobFile> blob_file_;
  mutable std::shared_ptr<Writer> writer_;

  // It is safe to not using std::atomic since the compaction filter, created
  // from a compaction filter factroy, will not be called from multiple threads.
  mutable uint64_t expired_count_ = 0;
  mutable uint64_t expired_size_ = 0;
  mutable uint64_t evicted_count_ = 0;
  mutable uint64_t evicted_size_ = 0;
};

class BlobIndexCompactionFilter : public BlobIndexCompactionFilterBase {
 public:
  BlobIndexCompactionFilter(
      BlobCompactionContext&& blob_comp_context,
      const CompactionFilter* _user_comp_filter,
      std::unique_ptr<const CompactionFilter> _user_comp_filter_from_factory,
      uint64_t current_time, Statistics* stats)
      : BlobIndexCompactionFilterBase(
            std::move(blob_comp_context), _user_comp_filter,
            std::move(_user_comp_filter_from_factory), current_time, stats) {}

  const char* Name() const override { return "BlobIndexCompactionFilter"; }
};

class BlobIndexCompactionFilterGC : public BlobIndexCompactionFilterBase {
 public:
  BlobIndexCompactionFilterGC(
      BlobCompactionContext&& blob_comp_context,
      BlobCompactionContextGC&& context_gc,
      const CompactionFilter* _user_comp_filter,
      std::unique_ptr<const CompactionFilter> _user_comp_filter_from_factory,
      uint64_t current_time, Statistics* stats)
      : BlobIndexCompactionFilterBase(
            std::move(blob_comp_context), _user_comp_filter,
            std::move(_user_comp_filter_from_factory), current_time, stats),
        context_gc_(std::move(context_gc)) {}

  ~BlobIndexCompactionFilterGC() override;

  const char* Name() const override { return "BlobIndexCompactionFilterGC"; }

  BlobDecision PrepareBlobOutput(const Slice& key, const Slice& existing_value,
                                 std::string* new_value) const override;

 private:
  bool OpenNewBlobFileWithStatsIfNeeded() const;

 private:
  BlobCompactionContextGC context_gc_;
  mutable BlobDBGarbageCollectionStats gc_stats_;
};

// Compaction filter factory; similarly to the filters above, it comes
// in two flavors, one that creates filters that support GC, and one
// that creates non-GC filters.
class BlobIndexCompactionFilterFactoryBase : public CompactionFilterFactory {
 public:
  BlobIndexCompactionFilterFactoryBase(BlobDBImpl* _blob_db_impl, Env* _env,
                                       const ColumnFamilyOptions& _cf_options,
                                       Statistics* _statistics)
      : user_comp_filter_(_cf_options.compaction_filter),
        user_comp_filter_factory_(_cf_options.compaction_filter_factory),
        blob_db_impl_(_blob_db_impl),
        env_(_env),
        statistics_(_statistics) {}

 protected:
  BlobDBImpl* blob_db_impl() const { return blob_db_impl_; }
  Env* env() const { return env_; }
  Statistics* statistics() const { return statistics_; }

 protected:
  const CompactionFilter* user_comp_filter_;
  std::shared_ptr<CompactionFilterFactory> user_comp_filter_factory_;

 private:
  BlobDBImpl* blob_db_impl_;
  Env* env_;
  Statistics* statistics_;
};

class BlobIndexCompactionFilterFactory
    : public BlobIndexCompactionFilterFactoryBase {
 public:
  BlobIndexCompactionFilterFactory(BlobDBImpl* _blob_db_impl, Env* _env,
                                   const ColumnFamilyOptions& _cf_options,
                                   Statistics* _statistics)
      : BlobIndexCompactionFilterFactoryBase(_blob_db_impl, _env, _cf_options,
                                             _statistics) {}

  const char* Name() const override {
    return "BlobIndexCompactionFilterFactory";
  }

  std::unique_ptr<CompactionFilter> CreateCompactionFilter(
      const CompactionFilter::Context& context) override;
};

class BlobIndexCompactionFilterFactoryGC
    : public BlobIndexCompactionFilterFactoryBase {
 public:
  BlobIndexCompactionFilterFactoryGC(BlobDBImpl* _blob_db_impl, Env* _env,
                                     const ColumnFamilyOptions& _cf_options,
                                     Statistics* _statistics)
      : BlobIndexCompactionFilterFactoryBase(_blob_db_impl, _env, _cf_options,
                                             _statistics) {}

  const char* Name() const override {
    return "BlobIndexCompactionFilterFactoryGC";
  }

  std::unique_ptr<CompactionFilter> CreateCompactionFilter(
      const CompactionFilter::Context& context) override;
};

}  // namespace blob_db
}  // namespace ROCKSDB_NAMESPACE
#endif  // ROCKSDB_LITE
