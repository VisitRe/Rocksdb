//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once
#include <stdint.h>

#include <memory>
#include <string>

#include "db/dbformat.h"
#include "rocksdb/flush_block_policy.h"
#include "rocksdb/table.h"
#include "rocksdb/utilities/options_type.h"

namespace rocksdb {

struct EnvOptions;

class BlockBasedTableBuilder;

// A class used to track actual bytes written from the tail in the recent SST
// file opens, and provide a suggestion for following open.
class TailPrefetchStats {
 public:
  void RecordEffectiveSize(size_t len);
  // 0 indicates no information to determine.
  size_t GetSuggestedPrefetchSize();

 private:
  const static size_t kNumTracked = 32;
  size_t records_[kNumTracked];
  port::Mutex mutex_;
  size_t next_ = 0;
  size_t num_records_ = 0;
};

class BlockBasedTableFactory : public TableFactory {
 private:
  static const std::string kBlockTablePrefix /* = rocksdb.table.block_based */;

 public:
  explicit BlockBasedTableFactory(
      const BlockBasedTableOptions& table_options = BlockBasedTableOptions());

  ~BlockBasedTableFactory() {}

  const char* Name() const override { return kBlockBasedTableName.c_str(); }

  Status NewTableReader(
      const TableReaderOptions& table_reader_options,
      std::unique_ptr<RandomAccessFileReader>&& file, uint64_t file_size,
      std::unique_ptr<TableReader>* table_reader,
      bool prefetch_index_and_filter_in_cache = true) const override;

  TableBuilder* NewTableBuilder(
      const TableBuilderOptions& table_builder_options,
      uint32_t column_family_id, WritableFileWriter* file) const override;

  bool IsDeleteRangeSupported() const override { return true; }

 protected:
  const void* GetOptionsPtr(const std::string& name) const override;
  const std::string& GetOptionsPrefix() const override {
    return kBlockTablePrefix;
  }
  // Validates the specified DB Options.
  Status Validate(const DBOptions& db_opts,
                  const ColumnFamilyOptions& cf_opts) const override;

#ifndef ROCKSDB_LITE
  Status SetUnknown(const DBOptions& db_opts, const std::string& name,
                    const std::string& value) override;
  bool IsUnknownEqual(const std::string& optName,
                      const OptionTypeInfo& type_info,
                      OptionsSanityCheckLevel sanity_check_level,
                      const char* thisAddr,
                      const char* thatAddr) const override;
  Status UnknownToString(uint32_t mode, const std::string& name,
                         std::string* value) const override;
  Status ParseOption(const OptionTypeInfo& opt_info, const DBOptions& db_opts,
                     void* opt_ptr, const std::string& opt_name,
                     const std::string& opt_value,
                     bool input_strings_escaped) override;
  const std::unordered_map<std::string, OptionsSanityCheckLevel>*
  GetOptionsSanityCheckLevel(const std::string& name) const override;

#endif
 private:
  BlockBasedTableOptions table_options_;
  mutable TailPrefetchStats tail_prefetch_stats_;
};

extern const std::string kHashIndexPrefixesBlock;
extern const std::string kHashIndexPrefixesMetadataBlock;
extern const std::string kPropTrue;
extern const std::string kPropFalse;

}  // namespace rocksdb
