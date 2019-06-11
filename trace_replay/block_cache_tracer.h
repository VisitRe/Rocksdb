//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include "monitoring/instrumented_mutex.h"
#include "rocksdb/env.h"
#include "rocksdb/options.h"
#include "rocksdb/trace_reader_writer.h"
#include "trace_replay/trace_replay.h"

namespace rocksdb {

enum BlockCacheLookupCaller : char {
  kUserGet = 1,
  kUserMGet = 2,
  kUserIterator = 3,
  kUserApproximateSize = 4,
  kPrefetch = 5,
  kCompaction = 6,
  // All callers should be added before kMaxBlockCacheLookupCaller.
  kMaxBlockCacheLookupCaller
};

// Lookup context for tracing block cache accesses.
// We trace block accesses at five places:
// 1. BlockBasedTable::GetFilter
// 2. BlockBasedTable::GetUncompressedDict.
// 3. BlockBasedTable::MaybeReadAndLoadToCache. (To trace access on data, index,
// and range deletion block.)
// 4. BlockBasedTable::Get. (To trace the referenced key and whether the
// referenced key exists in a fetched data block.)
// 5. BlockBasedTable::MultiGet. (To trace the referenced key and whether the
// referenced key exists in a fetched data block.)
// The context is created at:
// 1. BlockBasedTable::Get. (kUserGet)
// 2. BlockBasedTable::MultiGet. (kUserMGet)
// 3. BlockBasedTable::NewIterator. (either kUserIterator, kCompaction, or
// external SST ingestion calls this function.)
// 4. BlockBasedTable::Open. (kPrefetch)
// 5. Index/Filter::CacheDependencies. (kPrefetch)
// 6. BlockBasedTable::ApproximateOffsetOf. (kCompaction or
// kUserApproximateSize).
struct BlockCacheLookupContext {
  BlockCacheLookupContext(const BlockCacheLookupCaller& _caller)
      : caller(_caller) {}
  const BlockCacheLookupCaller caller;
  // Required information related to the lookup.
  bool is_cache_hit;
  bool no_insert;
  TraceType block_type;
  uint64_t block_size;

  // Required information only for tracing at BlockBasedTable::GET and
  // BlockBasedTable::MultiGet.
  std::string block_key;
  uint64_t num_keys_in_block;

  void FillLookupContext(bool _is_cache_hit, bool _no_insert,
                         TraceType _block_type, uint64_t _block_size) {
    is_cache_hit = _is_cache_hit;
    no_insert = _no_insert;
    block_type = _block_type;
    block_size = _block_size;
  }

  void FillLookupContext(bool _is_cache_hit, bool _no_insert,
                         TraceType _block_type, uint64_t _block_size,
                         const std::string& _block_key,
                         uint64_t _num_keys_in_block) {
    FillLookupContext(_is_cache_hit, _no_insert, _block_type, _block_size);
    block_key = _block_key;
    num_keys_in_block = _num_keys_in_block;
  }
};

enum Boolean : char { kTrue = 1, kFalse = 0 };

struct BlockCacheTraceRecord {
  // Required fields for all accesses.
  uint64_t access_timestamp;
  std::string block_key;
  TraceType block_type;
  uint64_t block_size;
  uint64_t cf_id;
  std::string cf_name;
  uint32_t level;
  uint64_t sst_fd_number;
  BlockCacheLookupCaller caller;
  Boolean is_cache_hit;
  Boolean no_insert;

  // Required fields for data block and user Get/Multi-Get only.
  std::string referenced_key;
  uint64_t referenced_data_size = 0;
  uint64_t num_keys_in_block = 0;
  Boolean is_referenced_key_exist_in_block = Boolean::kFalse;
};

struct BlockCacheTraceHeader {
  uint64_t start_time;
  uint32_t rocksdb_major_version;
  uint32_t rocksdb_minor_version;
};

// BlockCacheTraceWriter captures all RocksDB block cache accesses using a
// user-provided TraceWriter. Every RocksDB operation is written as a single
// trace. Each trace will have a timestamp and type, followed by the trace
// payload.
class BlockCacheTraceWriter {
 public:
  BlockCacheTraceWriter(Env* env, const TraceOptions& trace_options,
                        std::unique_ptr<TraceWriter>&& trace_writer);
  ~BlockCacheTraceWriter() = default;
  // No copy and move.
  BlockCacheTraceWriter(const BlockCacheTraceWriter&) = delete;
  BlockCacheTraceWriter& operator=(const BlockCacheTraceWriter&) = delete;
  BlockCacheTraceWriter(BlockCacheTraceWriter&&) = delete;
  BlockCacheTraceWriter& operator=(BlockCacheTraceWriter&&) = delete;

  // Pass Slice references to avoid copy.
  Status WriteBlockAccess(const BlockCacheTraceRecord& record,
                          const Slice& block_key, const Slice& cf_name,
                          const Slice& referenced_key);

  // Write a trace header at the beginning, typically on initiating a trace,
  // with some metadata like a magic number and RocksDB version.
  Status WriteHeader();

  static bool ShouldTraceReferencedKey(TraceType block_type,
                                       BlockCacheLookupCaller caller);

  static const std::string kUnknownColumnFamilyName;

 private:
  bool ShouldTrace(const BlockCacheTraceRecord& record) const;

  Env* env_;
  TraceOptions trace_options_;
  std::unique_ptr<TraceWriter> trace_writer_;
  /*Mutex to protect trace_writer_ */
  InstrumentedMutex trace_writer_mutex_;
};

// BlockCacheTraceReader helps read the trace file generated by
// BlockCacheTraceWriter using a user provided TraceReader.
class BlockCacheTraceReader {
 public:
  BlockCacheTraceReader(std::unique_ptr<TraceReader>&& reader);
  ~BlockCacheTraceReader() = default;
  // No copy and move.
  BlockCacheTraceReader(const BlockCacheTraceReader&) = delete;
  BlockCacheTraceReader& operator=(const BlockCacheTraceReader&) = delete;
  BlockCacheTraceReader(BlockCacheTraceReader&&) = delete;
  BlockCacheTraceReader& operator=(BlockCacheTraceReader&&) = delete;

  Status ReadHeader(BlockCacheTraceHeader* header);

  Status ReadAccess(BlockCacheTraceRecord* record);

 private:
  std::unique_ptr<TraceReader> trace_reader_;
};

}  // namespace rocksdb
