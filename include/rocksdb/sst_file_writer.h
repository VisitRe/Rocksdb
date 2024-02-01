//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once


#include <memory>
#include <string>

#include "rocksdb/env.h"
#include "rocksdb/options.h"
#include "rocksdb/table_properties.h"
#include "rocksdb/types.h"
#include "rocksdb/wide_columns.h"

#if defined(__GNUC__) || defined(__clang__)
#define ROCKSDB_DEPRECATED_FUNC __attribute__((__deprecated__))
#elif _WIN32
#define ROCKSDB_DEPRECATED_FUNC __declspec(deprecated)
#endif

namespace ROCKSDB_NAMESPACE {

class Comparator;

// ExternalSstFileInfo include information about sst files created
// using SstFileWriter.
struct ExternalSstFileInfo {
  ExternalSstFileInfo()
      : file_path(""),
        smallest_key(""),
        largest_key(""),
        smallest_range_del_key(""),
        largest_range_del_key(""),
        file_checksum(""),
        file_checksum_func_name(""),
        sequence_number(0),
        file_size(0),
        num_entries(0),
        num_range_del_entries(0),
        version(0) {}

  ExternalSstFileInfo(const std::string& _file_path,
                      const std::string& _smallest_key,
                      const std::string& _largest_key,
                      SequenceNumber _sequence_number, uint64_t _file_size,
                      uint64_t _num_entries, int32_t _version)
      : file_path(_file_path),
        smallest_key(_smallest_key),
        largest_key(_largest_key),
        smallest_range_del_key(""),
        largest_range_del_key(""),
        file_checksum(""),
        file_checksum_func_name(""),
        sequence_number(_sequence_number),
        file_size(_file_size),
        num_entries(_num_entries),
        num_range_del_entries(0),
        version(_version) {}

  std::string file_path;     // external sst file path
  std::string smallest_key;  // smallest user key in file
  std::string largest_key;   // largest user key in file
  std::string
      smallest_range_del_key;  // smallest range deletion user key in file
  std::string largest_range_del_key;  // largest range deletion user key in file
  std::string file_checksum;          // sst file checksum;
  std::string file_checksum_func_name;  // The name of file checksum function
  SequenceNumber sequence_number;       // sequence number of all keys in file
  uint64_t file_size;                   // file size in bytes
  uint64_t num_entries;                 // number of entries in file
  uint64_t num_range_del_entries;  // number of range deletion entries in file
  int32_t version;                 // file version
};

// SstFileWriter is used to create sst files that can be added to database later
// All keys in files generated by SstFileWriter will have sequence number = 0.
//
// This class is NOT thread-safe.
class SstFileWriter {
 public:
  // User can pass `column_family` to specify that the generated file will
  // be ingested into this column_family, note that passing nullptr means that
  // the column_family is unknown.
  // If invalidate_page_cache is set to true, SstFileWriter will give the OS a
  // hint that this file pages is not needed every time we write 1MB to the
  // file. To use the rate limiter an io_priority smaller than IO_TOTAL can be
  // passed.
  // The `skip_filters` option is DEPRECATED and could be removed in the
  // future. Use `BlockBasedTableOptions::filter_policy` to control filter
  // generation.
  SstFileWriter(const EnvOptions& env_options, const Options& options,
                ColumnFamilyHandle* column_family = nullptr,
                bool invalidate_page_cache = true,
                Env::IOPriority io_priority = Env::IOPriority::IO_TOTAL,
                bool skip_filters = false)
      : SstFileWriter(env_options, options, options.comparator, column_family,
                      invalidate_page_cache, io_priority, skip_filters) {}

  // Deprecated API
  SstFileWriter(const EnvOptions& env_options, const Options& options,
                const Comparator* user_comparator,
                ColumnFamilyHandle* column_family = nullptr,
                bool invalidate_page_cache = true,
                Env::IOPriority io_priority = Env::IOPriority::IO_TOTAL,
                bool skip_filters = false);

  ~SstFileWriter();

  // Prepare SstFileWriter to write into file located at "file_path".
  Status Open(const std::string& file_path);

  // Add a Put key with value to currently opened file (deprecated)
  // REQUIRES: user_key is after any previously added point (Put/Merge/Delete)
  //           key according to the comparator.
  // REQUIRES: comparator is *not* timestamp-aware.
  ROCKSDB_DEPRECATED_FUNC Status Add(const Slice& user_key, const Slice& value);

  // Add a Put key with value to currently opened file
  // REQUIRES: user_key is after any previously added point (Put/Merge/Delete)
  //           key according to the comparator.
  // REQUIRES: comparator is *not* timestamp-aware.
  Status Put(const Slice& user_key, const Slice& value);

  // Add a Put (key with timestamp, value) to the currently opened file
  // REQUIRES: user_key is after any previously added point (Put/Merge/Delete)
  //           key according to the comparator.
  // REQUIRES: timestamp's size is equal to what is expected by the comparator.
  // When Options.persist_user_defined_timestamps is set to false, the timestamp
  // part will not be included in SST file.
  Status Put(const Slice& user_key, const Slice& timestamp, const Slice& value);

  // Add a PutEntity (key with the wide-column entity defined by "columns") to
  // the currently opened file
  Status PutEntity(const Slice& user_key, const WideColumns& columns);

  // Add a Merge key with value to currently opened file
  // REQUIRES: user_key is after any previously added point (Put/Merge/Delete)
  //           key according to the comparator.
  // REQUIRES: comparator is *not* timestamp-aware.
  Status Merge(const Slice& user_key, const Slice& value);

  // Add a deletion key to currently opened file
  // REQUIRES: user_key is after any previously added point (Put/Merge/Delete)
  //           key according to the comparator.
  // REQUIRES: comparator is *not* timestamp-aware.
  Status Delete(const Slice& user_key);

  // Add a deletion key with timestamp to the currently opened file
  // REQUIRES: user_key is after any previously added point (Put/Merge/Delete)
  //           key according to the comparator.
  // REQUIRES: timestamp's size is equal to what is expected by the comparator.
  // When Options.persist_user_defined_timestamps is set to false, the timestamp
  // part will not be included in SST file.
  Status Delete(const Slice& user_key, const Slice& timestamp);

  // Add a range deletion tombstone to currently opened file. Such a range
  // deletion tombstone does NOT delete point (Put/Merge/Delete) keys in the
  // same file.
  //
  // Range deletion tombstones may be added in any order, both with respect to
  // each other and with respect to the point (Put/Merge/Delete) keys in the
  // same file.
  //
  // REQUIRES: The comparator orders `begin_key` at or before `end_key`
  // REQUIRES: comparator is *not* timestamp-aware.
  Status DeleteRange(const Slice& begin_key, const Slice& end_key);

  // Add a range deletion tombstone to currently opened file. Such a range
  // deletion tombstone does NOT delete point (Put/Merge/Delete) keys in the
  // same file.
  //
  // Range deletion tombstones may be added in any order, both with respect to
  // each other and with respect to the point (Put/Merge/Delete) keys in the
  // same file.
  //
  // REQUIRES: begin_key and end_key are user keys without timestamp.
  // REQUIRES: The comparator orders `begin_key` at or before `end_key`
  // REQUIRES: timestamp's size is equal to what is expected by the comparator.
  // When Options.persist_user_defined_timestamps is set to false, the timestamp
  // part will not be included in SST file.
  Status DeleteRange(const Slice& begin_key, const Slice& end_key,
                     const Slice& timestamp);

  // Finalize writing to sst file and close file.
  //
  // An optional ExternalSstFileInfo pointer can be passed to the function
  // which will be populated with information about the created sst file.
  Status Finish(ExternalSstFileInfo* file_info = nullptr);

  // Return the current file size.
  uint64_t FileSize();

 private:
  void InvalidatePageCache(bool closing);
  struct Rep;
  std::unique_ptr<Rep> rep_;
};
}  // namespace ROCKSDB_NAMESPACE
