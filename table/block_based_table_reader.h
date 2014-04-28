//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once

#include <stdint.h>
#include <memory>
#include <utility>
#include <string>

#include "rocksdb/statistics.h"
#include "rocksdb/status.h"
#include "rocksdb/table.h"
#include "table/table_reader.h"
#include "util/coding.h"

namespace rocksdb {

class Block;
class BlockHandle;
class Cache;
class FilterBlockReader;
class Footer;
class InternalKeyComparator;
class Iterator;
class RandomAccessFile;
class TableCache;
class TableReader;
class WritableFile;
struct BlockBasedTableOptions;
struct EnvOptions;
struct Options;
struct ReadOptions;

using std::unique_ptr;

// A Table is a sorted map from strings to strings.  Tables are
// immutable and persistent.  A Table may be safely accessed from
// multiple threads without external synchronization.
class BlockBasedTable : public TableReader {
 public:
  static const std::string kFilterBlockPrefix;

  // Attempt to open the table that is stored in bytes [0..file_size)
  // of "file", and read the metadata entries necessary to allow
  // retrieving data from the table.
  //
  // If successful, returns ok and sets "*table_reader" to the newly opened
  // table.  The client should delete "*table_reader" when no longer needed.
  // If there was an error while initializing the table, sets "*table_reader"
  // to nullptr and returns a non-ok status.
  //
  // *file must remain live while this Table is in use.
  static Status Open(const Options& db_options, const EnvOptions& env_options,
                     const BlockBasedTableOptions& table_options,
                     const InternalKeyComparator& internal_key_comparator,
                     unique_ptr<RandomAccessFile>&& file, uint64_t file_size,
                     unique_ptr<TableReader>* table_reader);

  bool PrefixMayMatch(const Slice& internal_key);

  // Returns a new iterator over the table contents.
  // The result of NewIterator() is initially invalid (caller must
  // call one of the Seek methods on the iterator before using it).
  Iterator* NewIterator(const ReadOptions&) override;

  Status Get(const ReadOptions& readOptions, const Slice& key,
             void* handle_context,
             bool (*result_handler)(void* handle_context,
                                    const ParsedInternalKey& k, const Slice& v,
                                    bool didIO),
             void (*mark_key_may_exist_handler)(void* handle_context) =
                 nullptr) override;

  // Given a key, return an approximate byte offset in the file where
  // the data for that key begins (or would begin if the key were
  // present in the file).  The returned value is in terms of file
  // bytes, and so includes effects like compression of the underlying data.
  // E.g., the approximate offset of the last key in the table will
  // be close to the file length.
  uint64_t ApproximateOffsetOf(const Slice& key) override;

  // Returns true if the block for the specified key is in cache.
  // REQUIRES: key is in this table.
  bool TEST_KeyInCache(const ReadOptions& options, const Slice& key);

  // Set up the table for Compaction. Might change some parameters with
  // posix_fadvise
  void SetupForCompaction() override;

  std::shared_ptr<const TableProperties> GetTableProperties() const override;

  ~BlockBasedTable();

  bool TEST_filter_block_preloaded() const;
  bool TEST_index_reader_preloaded() const;
  // Implementation of IndexReader will be exposed to internal cc file only.
  class IndexReader;

 private:
  template <class TValue>
  struct CachableEntry;

  struct Rep;
  Rep* rep_;
  bool compaction_optimized_;

  class BlockEntryIteratorState;
  static Iterator* NewDataBlockIterator(Rep* rep, const ReadOptions& ro,
      bool* didIO, const Slice& index_value);

  // For the following two functions:
  // if `no_io == true`, we will not try to read filter/index from sst file
  // were they not present in cache yet.
  CachableEntry<FilterBlockReader> GetFilter(bool no_io = false) const;

  // Get the iterator from the index reader.
  //
  // Note: ErrorIterator with Status::Incomplete shall be returned if all the
  // following conditions are met:
  //  1. We enabled table_options.cache_index_and_filter_blocks.
  //  2. index is not present in block cache.
  //  3. We disallowed any io to be performed, that is, read_options ==
  //     kBlockCacheTier
  Iterator* NewIndexIterator(const ReadOptions& read_options);

  // Read block cache from block caches (if set): block_cache and
  // block_cache_compressed.
  // On success, Status::OK with be returned and @block will be populated with
  // pointer to the block as well as its block handle.
  static Status GetDataBlockFromCache(
      const Slice& block_cache_key, const Slice& compressed_block_cache_key,
      Cache* block_cache, Cache* block_cache_compressed, Statistics* statistics,
      const ReadOptions& read_options,
      BlockBasedTable::CachableEntry<Block>* block);
  // Put a raw block (maybe compressed) to the corresponding block caches.
  // This method will perform decompression against raw_block if needed and then
  // populate the block caches.
  // On success, Status::OK will be returned; also @block will be populated with
  // uncompressed block and its cache handle.
  //
  // REQUIRES: raw_block is heap-allocated. PutDataBlockToCache() will be
  // responsible for releasing its memory if error occurs.
  static Status PutDataBlockToCache(
      const Slice& block_cache_key, const Slice& compressed_block_cache_key,
      Cache* block_cache, Cache* block_cache_compressed,
      const ReadOptions& read_options, Statistics* statistics,
      CachableEntry<Block>* block, Block* raw_block);

  // Calls (*handle_result)(arg, ...) repeatedly, starting with the entry found
  // after a call to Seek(key), until handle_result returns false.
  // May not make such a call if filter policy says that key is not present.
  friend class TableCache;
  friend class BlockBasedTableBuilder;

  void ReadMeta(const Footer& footer);
  void ReadFilter(const Slice& filter_handle_value);
  Status CreateIndexReader(IndexReader** index_reader);

  // Read the meta block from sst.
  static Status ReadMetaBlock(
      Rep* rep,
      std::unique_ptr<Block>* meta_block,
      std::unique_ptr<Iterator>* iter);

  // Create the filter from the filter block.
  static FilterBlockReader* ReadFilter(
      const Slice& filter_handle_value,
      Rep* rep,
      size_t* filter_size = nullptr);

  static void SetupCacheKeyPrefix(Rep* rep);

  explicit BlockBasedTable(Rep* rep)
      : rep_(rep), compaction_optimized_(false) {}

  // Generate a cache key prefix from the file
  static void GenerateCachePrefix(Cache* cc,
    RandomAccessFile* file, char* buffer, size_t* size);
  static void GenerateCachePrefix(Cache* cc,
    WritableFile* file, char* buffer, size_t* size);

  // The longest prefix of the cache key used to identify blocks.
  // For Posix files the unique ID is three varints.
  static const size_t kMaxCacheKeyPrefixSize = kMaxVarint64Length*3+1;

  // No copying allowed
  explicit BlockBasedTable(const TableReader&) = delete;
  void operator=(const TableReader&) = delete;
};

}  // namespace rocksdb
