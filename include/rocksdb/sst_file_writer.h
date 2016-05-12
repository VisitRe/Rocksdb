//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.

#pragma once
#include <string>
#include "rocksdb/env.h"
#include "rocksdb/immutable_options.h"
#include "rocksdb/types.h"

namespace rocksdb {

class Comparator;

// Table Properties that are specific to tables created by SstFileWriter.
struct ExternalSstFilePropertyNames {
  // value of this property is a fixed int32 number.
  static const std::string kVersion;
};

// ExternalSstFileInfo include information about sst files created
// using SstFileWriter
struct ExternalSstFileInfo {
  ExternalSstFileInfo() {}
  ExternalSstFileInfo(const std::string& _file_path,
                      const std::string& _smallest_key,
                      const std::string& _largest_key,
                      SequenceNumber _sequence_number, uint64_t _file_size,
                      int32_t _num_entries, int32_t _version)
      : file_path(_file_path),
        smallest_key(_smallest_key),
        largest_key(_largest_key),
        sequence_number(_sequence_number),
        file_size(_file_size),
        num_entries(_num_entries),
        version(_version) {}

  std::string file_path;           // external sst file path
  std::string smallest_key;        // smallest user key in file
  std::string largest_key;         // largest user key in file
  SequenceNumber sequence_number;  // sequence number of all keys in file
  uint64_t file_size;              // file size in bytes
  uint64_t num_entries;            // number of entries in file
  int32_t version;                 // file version
};

// SstFileWriter is used to create sst files that can be added to database later
// All keys in files generated by SstFileWriter will have sequence number = 0
class SstFileWriter {
 public:
  SstFileWriter(const EnvOptions& env_options,
                const ImmutableCFOptions& ioptions,
                const Comparator* user_comparator);

  ~SstFileWriter();

  // Prepare SstFileWriter to write into file located at "file_path".
  Status Open(const std::string& file_path);

  // Add key, value to currently opened file
  // REQUIRES: key is after any previously added key according to comparator.
  Status Add(const Slice& user_key, const Slice& value);

  // Finalize writing to sst file and close file.
  //
  // An optional ExternalSstFileInfo pointer can be passed to the function
  // which will be populated with information about the created sst file
  Status Finish(ExternalSstFileInfo* file_info = nullptr);

 private:
  class SstFileWriterPropertiesCollectorFactory;
  class SstFileWriterPropertiesCollector;
  struct Rep;
  Rep* rep_;
};
}  // namespace rocksdb
