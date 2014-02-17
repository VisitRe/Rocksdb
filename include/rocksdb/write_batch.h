// Copyright (c) 2013, Facebook, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// WriteBatch holds a collection of updates to apply atomically to a DB.
//
// The updates are applied in the order in which they are added
// to the WriteBatch.  For example, the value of "key" will be "v3"
// after the following batch is written:
//
//    batch.Put("key", "v1");
//    batch.Delete("key");
//    batch.Put("key", "v2");
//    batch.Put("key", "v3");
//
// Multiple threads can invoke const methods on a WriteBatch without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same WriteBatch must use
// external synchronization.

#ifndef STORAGE_ROCKSDB_INCLUDE_WRITE_BATCH_H_
#define STORAGE_ROCKSDB_INCLUDE_WRITE_BATCH_H_

#include <string>
#include "rocksdb/status.h"

namespace rocksdb {

class Slice;
struct SliceParts;

class WriteBatch {
 public:
  explicit WriteBatch(size_t reserved_bytes = 0);
  ~WriteBatch();

  // Store the mapping "key->value" in the database.
  void Put(uint32_t column_family_id, const Slice& key, const Slice& value);
  void Put(const Slice& key, const Slice& value) {
    Put(0, key, value);
  }

  // Variant of Put() that gathers output like writev(2).  The key and value
  // that will be written to the database are concatentations of arrays of
  // slices.
  void Put(uint32_t column_family_id, const SliceParts& key,
           const SliceParts& value);
  void Put(const SliceParts& key, const SliceParts& value) {
    Put(0, key, value);
  }

  // Merge "value" with the existing value of "key" in the database.
  // "key->merge(existing, value)"
  void Merge(uint32_t column_family_id, const Slice& key, const Slice& value);
  void Merge(const Slice& key, const Slice& value) {
    Merge(0, key, value);
  }

  // If the database contains a mapping for "key", erase it.  Else do nothing.
  void Delete(uint32_t column_family_id, const Slice& key);
  void Delete(const Slice& key) {
    Delete(0, key);
  }

  // Append a blob of arbitrary size to the records in this batch. The blob will
  // be stored in the transaction log but not in any other file. In particular,
  // it will not be persisted to the SST files. When iterating over this
  // WriteBatch, WriteBatch::Handler::LogData will be called with the contents
  // of the blob as it is encountered. Blobs, puts, deletes, and merges will be
  // encountered in the same order in thich they were inserted. The blob will
  // NOT consume sequence number(s) and will NOT increase the count of the batch
  //
  // Example application: add timestamps to the transaction log for use in
  // replication.
  void PutLogData(const Slice& blob);

  // Clear all updates buffered in this batch.
  void Clear();

  // Support for iterating over the contents of a batch.
  class Handler {
   public:
    virtual ~Handler();
    // default implementation will just call Put without column family for
    // backwards compatibility. If the column family is not default,
    // the function is noop
    virtual void PutCF(uint32_t column_family_id, const Slice& key,
                       const Slice& value) {
      if (column_family_id == 0) {
        Put(key, value);
      }
    }
    virtual void Put(const Slice& key, const Slice& value);
    // Merge and LogData are not pure virtual. Otherwise, we would break
    // existing clients of Handler on a source code level. The default
    // implementation of Merge simply throws a runtime exception.
    virtual void MergeCF(uint32_t column_family_id, const Slice& key,
                         const Slice& value) {
      if (column_family_id == 0) {
        Merge(key, value);
      }
    }
    virtual void Merge(const Slice& key, const Slice& value);
    // The default implementation of LogData does nothing.
    virtual void LogData(const Slice& blob);
    virtual void DeleteCF(uint32_t column_family_id, const Slice& key) {
      if (column_family_id == 0) {
        Delete(key);
      }
    }
    virtual void Delete(const Slice& key);
    // Continue is called by WriteBatch::Iterate. If it returns false,
    // iteration is halted. Otherwise, it continues iterating. The default
    // implementation always returns true.
    virtual bool Continue();
  };
  Status Iterate(Handler* handler) const;

  // Retrieve the serialized version of this batch.
  const std::string& Data() const { return rep_; }

  // Retrieve data size of the batch.
  size_t GetDataSize() const { return rep_.size(); }

  // Returns the number of updates in the batch
  int Count() const;

  // Constructor with a serialized string object
  explicit WriteBatch(std::string rep): rep_(rep) {}

 private:
  friend class WriteBatchInternal;

  std::string rep_;  // See comment in write_batch.cc for the format of rep_

  // Intentionally copyable
};

}  // namespace rocksdb

#endif  // STORAGE_ROCKSDB_INCLUDE_WRITE_BATCH_H_
