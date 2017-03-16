//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once
#include <stdint.h>
#include <string>
#include "db/dbformat.h"
#include "db/range_del_aggregator.h"
#include "rocksdb/db.h"
#include "rocksdb/iterator.h"
#include "util/arena.h"
#include "util/autovector.h"
#include "util/cf_options.h"

namespace rocksdb {

class Arena;
class DBIter;
class InternalIterator;

// Return a new iterator that converts internal keys (yielded by
// "*internal_iter") that were live at the specified "sequence" number
// into appropriate user keys.
extern Iterator* NewDBIterator(
    Env* env, const ImmutableCFOptions& options,
    const Comparator* user_key_comparator, InternalIterator* internal_iter,
    const SequenceNumber& sequence, uint64_t max_sequential_skip_in_iterations,
    uint64_t version_number, const Slice* iterate_upper_bound = nullptr,
    bool prefix_same_as_start = false, bool pin_data = false,
    bool total_order_seek = false,
    uint64_t max_tombstones_skip_in_iterations = 0);

// A wrapper iterator which wraps DB Iterator and the arena, with which the DB
// iterator is supposed be allocated. This class is used as an entry point of
// a iterator hierarchy whose memory can be allocated inline. In that way,
// accessing the iterator tree can be more cache friendly. It is also faster
// to allocate.
class ArenaWrappedDBIter : public Iterator {
 public:
  virtual ~ArenaWrappedDBIter();

  // Get the arena to be used to allocate memory for DBIter to be wrapped,
  // as well as child iterators in it.
  virtual Arena* GetArena() { return &arena_; }
  virtual RangeDelAggregator* GetRangeDelAggregator();

  // Set the DB Iterator to be wrapped

  virtual void SetDBIter(DBIter* iter);

  // Set the internal iterator wrapped inside the DB Iterator. Usually it is
  // a merging iterator.
  virtual void SetIterUnderDBIter(InternalIterator* iter);
  virtual bool Valid() const override;
  virtual void SeekToFirst() override;
  virtual void SeekToLast() override;
  virtual void Seek(const Slice& target) override;
  virtual void SeekForPrev(const Slice& target) override;
  virtual void Next() override;
  virtual void Prev() override;
  virtual Slice key() const override;
  virtual Slice value() const override;
  virtual Status status() const override;

  void RegisterCleanup(CleanupFunction function, void* arg1, void* arg2);
  virtual Status GetProperty(std::string prop_name, std::string* prop) override;

 private:
  DBIter* db_iter_;
  Arena arena_;
};

// Generate the arena wrapped iterator class.
extern ArenaWrappedDBIter* NewArenaWrappedDbIterator(
    Env* env, const ImmutableCFOptions& options,
    const Comparator* user_key_comparator, const SequenceNumber& sequence,
    uint64_t max_sequential_skip_in_iterations, uint64_t version_number,
    const Slice* iterate_upper_bound = nullptr,
    bool prefix_same_as_start = false, bool pin_data = false,
    bool total_order_seek = false,
    uint64_t max_tombstones_skip_in_iterations = 0);

}  // namespace rocksdb
