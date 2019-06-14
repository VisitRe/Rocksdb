//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <atomic>

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
};

enum Boolean : char { kTrue = 1, kFalse = 0 };

struct BlockCacheTraceRecord {
  // Required fields for all accesses.
  uint64_t access_timestamp;
  std::string block_key;
  TraceType block_type;
  uint64_t block_size;
  uint32_t cf_id;
  std::string cf_name;
  uint32_t level;
  uint32_t sst_fd_number;
  BlockCacheLookupCaller caller;
  Boolean is_cache_hit;
  Boolean no_insert;

  // Required fields for data block and user Get/Multi-Get only.
  std::string referenced_key;
  uint64_t num_keys_in_block = 0;
  Boolean is_referenced_key_exist_in_block = Boolean::kFalse;
};

struct BlockCacheTraceHeader {
  uint64_t start_time;
  uint32_t rocksdb_major_version;
  uint32_t rocksdb_minor_version;
};

bool ShouldTraceReferencedKey(const BlockCacheTraceRecord& record);

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

  Status WriteBlockAccess(const BlockCacheTraceRecord& record);

  // Write a trace header at the beginning, typically on initiating a trace,
  // with some metadata like a magic number and RocksDB version.
  Status WriteHeader();

 private:
  Env* env_;
  TraceOptions trace_options_;
  std::unique_ptr<TraceWriter> trace_writer_;
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

// A block cache tracer. It downsamples the accesses according to
// trace_options and uses BlockCacheTraceWriter to write the access record to
// the trace file.
class BlockCacheTracer {
 public:
  BlockCacheTracer();
  ~BlockCacheTracer();
  // No copy and move.
  BlockCacheTracer(const BlockCacheTracer&) = delete;
  BlockCacheTracer& operator=(const BlockCacheTracer&) = delete;
  BlockCacheTracer(BlockCacheTracer&&) = delete;
  BlockCacheTracer& operator=(BlockCacheTracer&&) = delete;

  // Start writing block cache accesses to the trace_writer.
  Status StartTrace(Env* env, const TraceOptions& trace_options,
                    std::unique_ptr<TraceWriter>&& trace_writer);

  // Stop writing block cache accesses to the trace_writer.
  void EndTrace();

  Status WriteBlockAccess(const BlockCacheTraceRecord& record);

 private:
  TraceOptions trace_options_;
  // A mutex protects the writer_.
  InstrumentedMutex trace_writer_mutex_;
  std::atomic<BlockCacheTraceWriter*> writer_;
};

}  // namespace rocksdb
