//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <stdint.h>
#include <atomic>
#include <mutex>
#include <set>
#include <vector>

#include "rocksdb/flush_manager.h"
#include "util/autovector.h"

namespace rocksdb {

class ColumnFamilyData;
class ColumnFamilySet;

// Unless otherwise noted, all methods on FlushScheduler should be called
// only with the DB mutex held or from a single-threaded recovery context.
class FlushScheduler {
 public:
  FlushScheduler() : head_(nullptr) {}

  // May be called from multiple threads at once, but not concurrent with
  // any other method calls on this instance
  void ScheduleFlush(ColumnFamilyData* cfd);

  // Removes and returns Ref()-ed column family. Client needs to Unref().
  // Filters column families that have been dropped.
  ColumnFamilyData* TakeNextColumnFamily();

  bool Empty();

  void Clear();

 private:
  struct Node {
    ColumnFamilyData* column_family;
    Node* next;
  };

  std::atomic<Node*> head_;
#ifndef NDEBUG
  std::mutex checking_mutex_;
  std::set<ColumnFamilyData*> checking_set_;
#endif  // NDEBUG
};

// TODO (yanqin) move to a different file.
//
// A flush request specifies the column families to flush as well as the
// largest memtable id to persist for each column family. Once all the
// memtables whose IDs are smaller than or equal to this per-column-family
// specified value, this flush request is considered to have completed its
// work of flushing this column family. After completing the work for all
// column families in this request, this flush is considered complete.
typedef std::vector<std::pair<ColumnFamilyData*, uint64_t>> FlushRequest;

class FlushManager {
 public:
  explicit FlushManager(ExternalFlushManager* external_manager)
      : external_manager_(external_manager) {}

  virtual ~FlushManager() {}

  virtual void OnManualFlush(ColumnFamilySet& column_family_set,
                             ColumnFamilyData* cfd,
                             std::atomic<bool>& cached_recoverable_state_empty,
                             autovector<ColumnFamilyData*>* cfds_picked,
                             std::vector<std::vector<uint32_t>>* to_flush) = 0;

  virtual void OnSwitchWAL(ColumnFamilySet& column_family_set,
                           uint64_t oldest_alive_log,
                           autovector<ColumnFamilyData*>* cfds_picked,
                           std::vector<std::vector<uint32_t>>* to_flush) = 0;

  virtual void OnHandleWriteBufferFull(
      ColumnFamilySet& column_family_set,
      autovector<ColumnFamilyData*>* cfds_picked,
      std::vector<std::vector<uint32_t>>* to_flush) = 0;

  virtual void OnScheduleFlushes(
      ColumnFamilySet& column_family_set, FlushScheduler& scheduler,
      autovector<ColumnFamilyData*>* cfds_picked,
      std::vector<std::vector<uint32_t>>* to_flush) = 0;

  void GenerateFlushRequests(ColumnFamilySet& column_family_set,
                             autovector<ColumnFamilyData*>& cfds,
                             const std::vector<std::vector<uint32_t>>& to_flush,
                             autovector<FlushRequest, 1>* requests);

 protected:
  void DedupColumnFamilies(ColumnFamilySet& column_family_set,
                           const std::vector<std::vector<uint32_t>>& to_flush,
                           autovector<ColumnFamilyData*>* unique_cfds);

 public:
  // external_manager_ is NOT owned by me.
  ExternalFlushManager* const external_manager_;
};

class DefaultFlushManager : public FlushManager {
 public:
  explicit DefaultFlushManager(ExternalFlushManager* external_manager)
      : FlushManager(external_manager) {}

  virtual ~DefaultFlushManager() {}

  virtual void OnManualFlush(
      ColumnFamilySet& /* column_family_set */, ColumnFamilyData* cfd,
      std::atomic<bool>& cached_recoverable_state_empty,
      autovector<ColumnFamilyData*>* cfds_picked,
      std::vector<std::vector<uint32_t>>* to_flush) override;

  virtual void OnSwitchWAL(
      ColumnFamilySet& column_family_set, uint64_t oldest_alive_log,
      autovector<ColumnFamilyData*>* cfds_picked,
      std::vector<std::vector<uint32_t>>* to_flush) override;

  virtual void OnHandleWriteBufferFull(
      ColumnFamilySet& column_family_set,
      autovector<ColumnFamilyData*>* cfds_picked,
      std::vector<std::vector<uint32_t>>* to_flush) override;

  virtual void OnScheduleFlushes(
      ColumnFamilySet& column_family_set, FlushScheduler& scheduler,
      autovector<ColumnFamilyData*>* cfds_picked,
      std::vector<std::vector<uint32_t>>* to_flush) override;
};

}  // namespace rocksdb
