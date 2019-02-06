// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "rocksdb/stats_history.h"

namespace rocksdb {

class InMemoryStatsHistoryIterator final : public StatsHistoryIterator {
 public:
  InMemoryStatsHistoryIterator(uint64_t start_time, uint64_t end_time,
                               DBImpl* db_impl)
      : start_time_(start_time),
        end_time_(end_time),
        valid_(true),
        db_impl_(db_impl) {
    AdvanceIteratorByTime(start_time_, end_time_);
  }
  virtual ~InMemoryStatsHistoryIterator();
  virtual bool Valid() const override;
  virtual Status status() const override;

  virtual void Next() override;
  virtual uint64_t GetStatsTime() const override;

  virtual const std::map<std::string, uint64_t>& GetStatsMap()
      const override;

 private:
  // advance the iterator to the next stats history record with timestamp
  // between [start_time, end_time)
  void AdvanceIteratorByTime(uint64_t start_time, uint64_t end_time);

  // No copying allowed
  InMemoryStatsHistoryIterator(const InMemoryStatsHistoryIterator&) = delete;
  InMemoryStatsHistoryIterator(InMemoryStatsHistoryIterator&&) = delete;
  void operator=(const InMemoryStatsHistoryIterator&) = delete;

  uint64_t time_;
  uint64_t start_time_;
  uint64_t end_time_;
  std::map<std::string, uint64_t> stats_map_;
  Status status_;
  bool valid_;
  DBImpl* db_impl_;
};

}  // namespace rocksdb
