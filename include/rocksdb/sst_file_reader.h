//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once


#include "rocksdb/iterator.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "rocksdb/table_properties.h"

namespace ROCKSDB_NAMESPACE {

// SstFileReader is used to read sst files that are generated by DB or
// SstFileWriter.
class SstFileReader {
 public:
  SstFileReader(const Options& options);

  ~SstFileReader();

  // Prepares to read from the file located at "file_path".
  Status Open(const std::string& file_path);

  // Returns a new iterator over the table contents as a DB iterator, a.k.a
  // a `DBIter` that iterates logically visible entries, for example, a delete
  // entry is not logically visible.
  // Most read options provide the same control as we read from DB.
  // If "snapshot" is nullptr, the iterator returns only the latest keys.
  Iterator* NewIterator(const ReadOptions& options);

  // Returns a new iterator over the table contents as a raw table iterator,
  // a.k.a a `TableIterator`that iterates all point data entries in the table
  // including logically invisible entries like delete entries.
  // This API is intended to provide a programmatic way to observe SST files
  // created by a DB, to be used by third party tools. DB optimization
  // capabilities like filling cache, read ahead are disabled.
  std::unique_ptr<Iterator> NewTableIterator();

  std::shared_ptr<const TableProperties> GetTableProperties() const;

  // Verifies whether there is corruption in this table.
  // For the default BlockBasedTable, this will verify the block
  // checksum of each block.
  Status VerifyChecksum(const ReadOptions& /*read_options*/);

  // TODO: plumb Env::IOActivity, Env::IOPriority
  Status VerifyChecksum() { return VerifyChecksum(ReadOptions()); }

  // Verify that the number of entries in the table matches table property.
  // A Corruption status is returned if they do not match.
  Status VerifyNumEntries(const ReadOptions& /*read_options*/);

 private:
  struct Rep;
  std::unique_ptr<Rep> rep_;
};

}  // namespace ROCKSDB_NAMESPACE
