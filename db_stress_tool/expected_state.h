//  Copyright (c) 2021-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <stdint.h>

#include <atomic>
#include <memory>

#include "rocksdb/env.h"
#include "rocksdb/rocksdb_namespace.h"

namespace ROCKSDB_NAMESPACE {

// An `ExpectedState` provides read/write access to expected values for every
// key.
class ExpectedState {
 public:
  explicit ExpectedState(size_t max_key, size_t num_column_families);

  virtual ~ExpectedState() {}

  virtual Status Open() = 0;

  void ClearColumnFamily(int cf);

  void Put(int cf, int64_t key, uint32_t value_base, bool pending);

  uint32_t Get(int cf, int64_t key) const;

  bool Delete(int cf, int64_t key, bool pending);

  bool SingleDelete(int cf, int64_t key, bool pending);

  int DeleteRange(int cf, int64_t begin_key, int64_t end_key, bool pending);

  bool Exists(int cf, int64_t key);

 private:
  std::atomic<uint32_t>& Value(int cf, int64_t key) const {
    return values_[cf * max_key_ + key];
  }

  const size_t max_key_;
  const size_t num_column_families_;

 protected:
  size_t GetValuesLen() const {
    return sizeof(std::atomic<uint32_t>) * num_column_families_ * max_key_;
  }

  void Reset();

  std::atomic<uint32_t>* values_;
};

// A `FileExpectedState` implements `ExpectedState` backed by a file.
class FileExpectedState : public ExpectedState {
 public:
  explicit FileExpectedState(std::string expected_state_file_path,
                             size_t max_key, size_t num_column_families);

  Status Open() override;

 private:
  const std::string expected_state_file_path_;
  std::unique_ptr<MemoryMappedFileBuffer> expected_state_mmap_buffer_;
};

// An `AnonExpectedState` implements `ExpectedState` backed by a memory
// allocation.
class AnonExpectedState : public ExpectedState {
 public:
  explicit AnonExpectedState(size_t max_key, size_t num_column_families);

  Status Open() override;

 private:
  std::unique_ptr<std::atomic<uint32_t>[]> values_allocation_;
};

// An `ExpectedStateManager` manages a directory containing data about the
// expected state of the database. It exposes operations for reading and
// modifying the latest expected state.
class ExpectedStateManager {
 public:
  explicit ExpectedStateManager(std::string expected_state_dir_path,
                                size_t max_key, size_t num_column_families);

  ~ExpectedStateManager();

  // The following APIs are not thread-safe and require external synchronization
  // for the entire object.
  Status Open();

  // The following APIs are not thread-safe and require external synchronization
  // for the affected keys only. For example, `Put("a", ...)` and
  // `Put("b", ...)` could be executed in parallel with no external
  // synchronization.
  void ClearColumnFamily(int cf) { return latest_->ClearColumnFamily(cf); }

  void Put(int cf, int64_t key, uint32_t value_base, bool pending) {
    return latest_->Put(cf, key, value_base, pending);
  }

  uint32_t Get(int cf, int64_t key) const { return latest_->Get(cf, key); }

  bool Delete(int cf, int64_t key, bool pending) {
    return latest_->Delete(cf, key, pending);
  }

  bool SingleDelete(int cf, int64_t key, bool pending) {
    return latest_->SingleDelete(cf, key, pending);
  }

  int DeleteRange(int cf, int64_t begin_key, int64_t end_key, bool pending) {
    return latest_->DeleteRange(cf, begin_key, end_key, pending);
  }

  bool Exists(int cf, int64_t key) { return latest_->Exists(cf, key); }

 private:
  static const std::string kLatestFilename;

  const std::string expected_state_dir_path_;
  const size_t max_key_;
  const size_t num_column_families_;
  std::unique_ptr<ExpectedState> latest_;
};

}  // namespace ROCKSDB_NAMESPACE
