//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <map>
#include <vector>

#include "rocksdb/env.h"
#include "trace_replay/block_cache_tracer.h"

namespace rocksdb {

// Statistics of a block.
struct BlockStats {
  uint64_t num_accesses = 0;
  uint64_t block_size = 0;
  uint64_t first_access_time = 0;
  uint64_t last_access_time = 0;
  uint64_t num_keys = 0;
  std::map<std::string, uint64_t> key_num_access_map;
  uint64_t num_referenced_key_not_exist = 0;
  std::map<BlockCacheLookupCaller, uint64_t> caller_num_access_map;

  void AddAccess(const BlockCacheTraceRecord& access) {
    if (first_access_time == 0) {
      first_access_time = access.access_timestamp;
    }
    last_access_time = access.access_timestamp;
    block_size = access.block_size;
    caller_num_access_map[access.caller]++;
    num_accesses++;
    if (ShouldTraceReferencedKey(access)) {
      num_keys = access.num_keys_in_block;
      key_num_access_map[access.referenced_key]++;
      if (access.is_referenced_key_exist_in_block == Boolean::kFalse) {
        num_referenced_key_not_exist++;
      }
    }
  }
};

// A set of blocks of a block type.
struct BlockTypeStats {
  std::map<std::string, BlockStats> block_stats_map;
};

// A set of blocks in a SST file.
struct SSTFileStats {
  uint32_t level;
  std::map<TraceType, BlockTypeStats> block_type_stats_map;
};

// A set of SST files in a column family.
struct ColumnFamilyStats {
  std::map<uint64_t, SSTFileStats> fd_stats_map;
};

class BlockCacheTraceAnalyzer {
 public:
  BlockCacheTraceAnalyzer(const std::string& trace_file_path);

  // It reads all access records in the given trace_file, maintains the stats of
  // a block, and aggregates the information by block type, sst file, and column
  // family. Subsequently, the caller may call Print* functions to print
  // statistics.
  Status Analyze();

  // Print a summary of statistics of the trace, e.g.,
  // Number of files: 2 Number of blocks: 50 Number of accesses: 50
  // Number of Index blocks: 10
  // Number of Filter blocks: 10
  // Number of Data blocks: 10
  // Number of UncompressionDict blocks: 10
  // Number of RangeDeletion blocks: 10
  // ***************************************************************
  // Caller Get: Number of accesses 10
  // Caller Get: Number of accesses per level break down
  //          Level 1: Number of accesses: 10
  // Caller Get: Number of accesses per block type break down
  //          Block Type Index: Number of accesses: 2
  //          Block Type Filter: Number of accesses: 2
  //          Block Type Data: Number of accesses: 2
  //          Block Type UncompressionDict: Number of accesses: 2
  //          Block Type RangeDeletion: Number of accesses: 2
  void PrintStatsSummary();

  // Print block size distribution.
  void PrintBlockSizeStats();

  // Print access count distribution.
  void PrintAccessCountStats();

  std::map<std::string, ColumnFamilyStats>& Test_cf_stats_map() {
    return cf_stats_map_;
  }

 private:
  void RecordAccess(const BlockCacheTraceRecord& access);

  rocksdb::Env* env_;
  std::string trace_file_path_;
  BlockCacheTraceHeader header_;
  std::map<std::string, ColumnFamilyStats> cf_stats_map_;
};

}  // namespace rocksdb
