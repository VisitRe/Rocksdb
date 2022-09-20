//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once

#include "db/range_del_aggregator.h"
#include "rocksdb/slice.h"
#include "rocksdb/types.h"

namespace ROCKSDB_NAMESPACE {

class Arena;
class ArenaWrappedDBIter;
class InternalKeyComparator;

template <class TValue>
class InternalIteratorBase;
using InternalIterator = InternalIteratorBase<Slice>;

// Return an iterator that provided the union of the data in
// children[0,n-1].  Takes ownership of the child iterators and
// will delete them when the result iterator is deleted.
//
// The result does no duplicate suppression.  I.e., if a particular
// key is present in K child iterators, it will be yielded K times.
//
// REQUIRES: n >= 0
extern InternalIterator* NewMergingIterator(
    const InternalKeyComparator* comparator, InternalIterator** children, int n,
    Arena* arena = nullptr, bool prefix_seek_mode = false);

class MergingIterator;

// A builder class to build a merging iterator by adding iterators one by one.
class MergeIteratorBuilder {
 public:
  // comparator: the comparator used in merging comparator
  // arena: where the merging iterator needs to be allocated from.
  explicit MergeIteratorBuilder(const InternalKeyComparator* comparator,
                                Arena* arena, bool prefix_seek_mode = false);
  ~MergeIteratorBuilder();

  // Add iter to the merging iterator.
  void AddIterator(InternalIterator* iter);

  // Add a range tombstone iterator to underlying merge iterator.
  // See MergingIterator::AddRangeTombstoneIterator() for more detail.
  //
  // If `iter_ptr` is not nullptr, *iter_ptr will be set to where the merging
  // iterator stores `iter` when MergeIteratorBuilder::Finish() is called. This
  // is used by level iterator to update range tombstone iters when switching to
  // a different SST file.
  void AddRangeTombstoneIterator(
      TruncatedRangeDelIterator* iter,
      TruncatedRangeDelIterator*** iter_ptr = nullptr);

  // Get arena used to build the merging iterator. It is called one a child
  // iterator needs to be allocated.
  Arena* GetArena() { return arena; }

  // Return the result merging iterator.
  // If db_iter is not nullptr, then db_iter->SetMemtableRangetombstoneIter()
  // will be called with pointer to where the merging iterator
  // stores the memtable range tombstone iterator.
  // This is used for DB iterator to refresh memtable range tombstones.
  InternalIterator* Finish(ArenaWrappedDBIter* db_iter = nullptr);

 private:
  MergingIterator* merge_iter;
  InternalIterator* first_iter;
  bool use_merging_iter;
  Arena* arena;
  // Used to set LevelIterator.range_tombstone_iter_.
  // See AddRangeTombstoneIterator() implementation for more detail.
  std::vector<std::pair<size_t, TruncatedRangeDelIterator***>>
      range_del_iter_ptrs_;
};

}  // namespace ROCKSDB_NAMESPACE
