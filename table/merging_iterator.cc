//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/merging_iterator.h"

#include "db/arena_wrapped_db_iter.h"
#include "db/dbformat.h"
#include "db/pinned_iterators_manager.h"
#include "memory/arena.h"
#include "monitoring/perf_context_imp.h"
#include "rocksdb/comparator.h"
#include "rocksdb/iterator.h"
#include "rocksdb/options.h"
#include "table/internal_iterator.h"
#include "table/iter_heap.h"
#include "table/iterator_wrapper.h"
#include "test_util/sync_point.h"
#include "util/autovector.h"
#include "util/heap.h"
#include "util/stop_watch.h"

namespace ROCKSDB_NAMESPACE {
// Without anonymous namespace here, we fail the warning -Wmissing-prototypes
namespace {
using MergerMaxIterHeap = BinaryHeap<IteratorWrapper*, MaxIteratorComparator>;
using MergerMinIterHeap = BinaryHeap<IteratorWrapper*, MinIteratorComparator>;
}  // namespace

class MergingIterator : public InternalIterator {
 public:
  MergingIterator(const InternalKeyComparator* comparator,
                  InternalIterator** children, int n, bool is_arena_mode,
                  bool prefix_seek_mode)
      : is_arena_mode_(is_arena_mode),
        prefix_seek_mode_(prefix_seek_mode),
        direction_(kForward),
        comparator_(comparator),
        current_(nullptr),
        minHeap_(comparator_),
        pinned_iters_mgr_(nullptr) {
    children_.resize(n);
    for (int i = 0; i < n; i++) {
      children_[i].Set(children[i]);
    }
  }

  void considerStatus(Status s) {
    if (!s.ok() && status_.ok()) {
      status_ = s;
    }
  }

  virtual void AddIterator(InternalIterator* iter) {
    children_.emplace_back(iter);
    if (pinned_iters_mgr_) {
      iter->SetPinnedItersMgr(pinned_iters_mgr_);
    }
    // Invalidate to ensure `Seek*()` is called to construct the heaps before
    // use.
    current_ = nullptr;
  }

  // Merging iterator can optionally process range deletions: if a key is
  // covered by a range deletion, the merging iterator will not output it but
  // skip it.
  //
  // Add next range tombstone iterator to this merging iterator.
  // There must be either no range tombstone iterator, or same number of
  // range tombstone iterators as point iterators after all range tombstone
  // iters are added. The i-th added range tombstone iterator and the i-th point
  // iterator must point to the same sorted run.
  void AddRangeTombstoneIterator(TruncatedRangeDelIterator* iter) {
    child_range_tombstones_.emplace_back(iter);
  }

  ~MergingIterator() override {
    for (auto child : child_range_tombstones_) {
      delete child;
    }

    for (auto& child : children_) {
      child.DeleteIter(is_arena_mode_);
    }
    status_.PermitUncheckedError();
  }

  bool Valid() const override { return current_ != nullptr && status_.ok(); }

  Status status() const override { return status_; }

  void SeekToFirst() override {
    ClearHeaps();
    status_ = Status::OK();
    for (auto& child : children_) {
      child.SeekToFirst();
      AddToMinHeapOrCheckStatus(&child);
    }
    for (auto& range_tombstone_iter : child_range_tombstones_) {
      if (range_tombstone_iter != nullptr) {
        // nullptr means no tombstones for this level
        range_tombstone_iter->SeekToFirst();
      }
    }

    if (!child_range_tombstones_.empty()) {
      // Skip range tombstone covered keys
      FindNextVisibleEntry();
    }
    direction_ = kForward;
    current_ = CurrentForward();
  }

  void SeekToLast() override {
    ClearHeaps();
    InitMaxHeap();
    status_ = Status::OK();
    for (auto& child : children_) {
      child.SeekToLast();
      AddToMaxHeapOrCheckStatus(&child);
    }
    for (auto& range_tombstone_iter : child_range_tombstones_) {
      if (range_tombstone_iter != nullptr) {
        range_tombstone_iter->SeekToLast();
      }
    }

    if (!child_range_tombstones_.empty()) {
      // Skip range tombstone covered keys
      FindPrevUserEntry();
    }
    direction_ = kReverse;
    current_ = CurrentReverse();
  }

  // Position this merging iterator at the first key >= target (internal key).
  // If range tombstones are present, keys covered by range tombstones are
  // skipped, and this merging iter points to the first non-range-deleted key >=
  // target after Seek(). If !Valid() and status().ok() then end of the iterator
  // is reached.
  //
  // Internally, this involves positioning all child iterators at the first key
  // >= target. If range tombstones are present, we apply a similar technique of
  // cascading seek as in Pebble (https://github.com/cockroachdb/pebble).
  // Specifically, if there is a range tombstone that covers the target key at
  // level L, then we know this range tombstone covers the range [target, range
  // tombstone end) for all levels > L. So for all levels > L, we can do seek on
  // the range tombstone end key instead of target. This optimization is applied
  // at each level and hence the name "cascading seek". After a round of
  // (cascading) seeks, the top of the heap is checked to see if it is covered
  // by a range tombstone (see FindNextVisibleEntry() for more detail), and
  // advanced if so. The process is repeated until a visible key is at the top
  // of the heap.
  // TODO: consider user defined timestamp?
  void Seek(const Slice& target) override {
    assert(child_range_tombstones_.empty() ||
           child_range_tombstones_.size() == children_.size());
    SeekImpl(target);
    if (!child_range_tombstones_.empty()) {
      // Skip range tombstone covered keys
      FindNextVisibleEntry();
    }

    direction_ = kForward;

    {
      PERF_TIMER_GUARD(seek_min_heap_time);
      current_ = CurrentForward();
    }
  }

  void SeekForPrev(const Slice& target) override {
    assert(child_range_tombstones_.empty() ||
           child_range_tombstones_.size() == children_.size());
    SeekForPrevImpl(target);
    if (!child_range_tombstones_.empty()) {
      // Skip range tombstone covered keys
      FindPrevUserEntry();
    }

    direction_ = kReverse;
    {
      PERF_TIMER_GUARD(seek_max_heap_time);
      current_ = CurrentReverse();
    }
  }

  void Next() override {
    assert(Valid());

    // Ensure that all children are positioned after key().
    // If we are moving in the forward direction, it is already
    // true for all of the non-current children since current_ is
    // the smallest child and key() == current_->key().
    if (direction_ != kForward) {
      // The loop advanced all non-current children to be > key() so current_
      // should still be strictly the smallest key.
      SwitchToForward();
    }

    // For the heap modifications below to be correct, current_ must be the
    // current top of the heap.
    assert(current_ == CurrentForward());

    // as the current points to the current record. move the iterator forward.
    current_->Next();
    if (current_->Valid()) {
      // current is still valid after the Next() call above.  Call
      // replace_top() to restore the heap property.  When the same child
      // iterator yields a sequence of keys, this is cheap.
      assert(current_->status().ok());
      minHeap_.replace_top(current_);
    } else {
      // current stopped being valid, remove it from the heap.
      considerStatus(current_->status());
      minHeap_.pop();
    }

    if (!child_range_tombstones_.empty()) {
      FindNextVisibleEntry();
    }
    current_ = CurrentForward();
  }

  bool NextAndGetResult(IterateResult* result) override {
    Next();
    bool is_valid = Valid();
    if (is_valid) {
      result->key = key();
      result->bound_check_result = UpperBoundCheckResult();
      result->value_prepared = current_->IsValuePrepared();
    }
    return is_valid;
  }

  void Prev() override {
    assert(Valid());
    // Ensure that all children are positioned before key().
    // If we are moving in the reverse direction, it is already
    // true for all of the non-current children since current_ is
    // the largest child and key() == current_->key().
    if (direction_ != kReverse) {
      // Otherwise, retreat the non-current children.  We retreat current_
      // just after the if-block.
      SwitchToBackward();
    }

    // For the heap modifications below to be correct, current_ must be the
    // current top of the heap.
    assert(current_ == CurrentReverse());

    current_->Prev();
    if (current_->Valid()) {
      // current is still valid after the Prev() call above.  Call
      // replace_top() to restore the heap property.  When the same child
      // iterator yields a sequence of keys, this is cheap.
      assert(current_->status().ok());
      maxHeap_->replace_top(current_);
    } else {
      // current stopped being valid, remove it from the heap.
      considerStatus(current_->status());
      maxHeap_->pop();
    }

    if (!child_range_tombstones_.empty()) {
      // Skip range tombstone covered keys
      FindPrevUserEntry();
    }
    current_ = CurrentReverse();
  }

  Slice key() const override {
    assert(Valid());
    return current_->key();
  }

  Slice value() const override {
    assert(Valid());
    return current_->value();
  }

  bool PrepareValue() override {
    assert(Valid());
    if (current_->PrepareValue()) {
      return true;
    }

    considerStatus(current_->status());
    assert(!status_.ok());
    return false;
  }

  // Here we simply relay MayBeOutOfLowerBound/MayBeOutOfUpperBound result
  // from current child iterator. Potentially as long as one of child iterator
  // report out of bound is not possible, we know current key is within bound.

  bool MayBeOutOfLowerBound() override {
    assert(Valid());
    return current_->MayBeOutOfLowerBound();
  }

  IterBoundCheck UpperBoundCheckResult() override {
    assert(Valid());
    return current_->UpperBoundCheckResult();
  }

  void SetPinnedItersMgr(PinnedIteratorsManager* pinned_iters_mgr) override {
    pinned_iters_mgr_ = pinned_iters_mgr;
    for (auto& child : children_) {
      child.SetPinnedItersMgr(pinned_iters_mgr);
    }
  }

  bool IsKeyPinned() const override {
    assert(Valid());
    return pinned_iters_mgr_ && pinned_iters_mgr_->PinningEnabled() &&
           current_->IsKeyPinned();
  }

  bool IsValuePinned() const override {
    assert(Valid());
    return pinned_iters_mgr_ && pinned_iters_mgr_->PinningEnabled() &&
           current_->IsValuePinned();
  }

 private:
  friend class MergeIteratorBuilder;
  // Clears heaps for both directions, used when changing direction or seeking
  void ClearHeaps();
  // Ensures that maxHeap_ is initialized when starting to go in the reverse
  // direction
  void InitMaxHeap();

  // Advance this merging iterator until the current key (top of min heap) is
  // not covered by any range tombstone or that there is no more keys (heap is
  // empty). After this call, if Valid(), current_ points to the next key that
  // is not covered by any range tombstone.
  void FindNextVisibleEntry();
  void FindPrevUserEntry();

  void SeekImpl(const Slice& target, size_t starting_level = 0,
                bool range_tombstone_reseek = false);

  // Seek to fist key <= target key (internal key) for
  // children_[starting_level:].
  void SeekForPrevImpl(const Slice& target, size_t starting_level = 0,
                       bool range_tombstone_reseek = false);

  bool is_arena_mode_;
  bool prefix_seek_mode_;
  // Which direction is the iterator moving?
  enum Direction : uint8_t { kForward, kReverse };
  Direction direction_;
  const InternalKeyComparator* comparator_;
  // Uses vector instead of autovector to make GetChildIndex() work.
  // We could also use an autovector with larger reserved size.
  std::vector<IteratorWrapper> children_;
  // child_range_tombstones_[i] contains range tombstones in sorted run
  // that corresponds to children_[i].
  // child_range_tombstones_.empty() means not handling range tombstones.
  // child_range_tombstones[i] == nullptr means a sorted run does not have range
  // tombstones.
  std::vector<TruncatedRangeDelIterator*> child_range_tombstones_;
  // Checks if top of the heap (current key) is covered by a range tombstone by
  // It current key is covered by some range tombstone, its iter is advanced and
  // heap property is maintained. Returns whether top of heap is deleted.
  bool IsNextDeleted();
  bool IsPrevDeleted();

  // Return the index of child in children_.
  // REQUIRES: child in children_.
  size_t GetChildIndex(IteratorWrapper* child);

  // Cached pointer to child iterator with the current key, or nullptr if no
  // child iterators are valid.  This is the top of minHeap_ or maxHeap_
  // depending on the direction.
  IteratorWrapper* current_;
  // If any of the children have non-ok status, this is one of them.
  Status status_;
  MergerMinIterHeap minHeap_;

  // Max heap is used for reverse iteration, which is way less common than
  // forward.  Lazily initialize it to save memory.
  std::unique_ptr<MergerMaxIterHeap> maxHeap_;
  PinnedIteratorsManager* pinned_iters_mgr_;

  // In forward direction, process a child that is not in the min heap.
  // If valid, add to the min heap. Otherwise, check status.
  void AddToMinHeapOrCheckStatus(IteratorWrapper*);

  // In backward direction, process a child that is not in the max heap.
  // If valid, add to the min heap. Otherwise, check status.
  void AddToMaxHeapOrCheckStatus(IteratorWrapper*);

  void SwitchToForward();

  // Switch the direction from forward to backward without changing the
  // position. Iterator should still be valid.
  void SwitchToBackward();

  IteratorWrapper* CurrentForward() const {
    assert(direction_ == kForward);
    return !minHeap_.empty() ? minHeap_.top() : nullptr;
  }

  IteratorWrapper* CurrentReverse() const {
    assert(direction_ == kReverse);
    assert(maxHeap_);
    return !maxHeap_->empty() ? maxHeap_->top() : nullptr;
  }
};

// Seek to fist key >= target key (internal key) for children_[starting_level:].
// Cascading seek optimizations are applied if range tombstones are present (see
// comment above Seek() for more).
//
// range_tombstone_reseek: whether this Seek is to some range tombstone end and
// is part of a "cascading seek". This is used for recoding relevant
// perf_context.
void MergingIterator::SeekImpl(const Slice& target, size_t starting_level,
                               bool range_tombstone_reseek) {
  ClearHeaps();
  status_ = Status::OK();
  IterKey current_search_key;
  current_search_key.SetInternalKey(target, false /* copy */);
  // (level, target) pairs
  autovector<std::pair<size_t, std::string>> pinned_prefetched_target;

  for (auto level = starting_level; level < children_.size(); ++level) {
    {
      PERF_TIMER_GUARD(seek_child_seek_time);
      children_[level].Seek(current_search_key.GetInternalKey());
    }

    if (range_tombstone_reseek) {
      // we are seeking to end of some range tombstone from a newer sorted run
      PERF_COUNTER_ADD(internal_range_del_reseek_count, 1);
    }

    PERF_COUNTER_ADD(seek_child_seek_count, 1);

    if (!child_range_tombstones_.empty()) {
      // avoids copying target key for async requests in range tombstone free
      // path
      if (children_[level].status().IsTryAgain()) {
        // search target might change to some range tombstone end key, so
        // we need to remember them for async requests.
        pinned_prefetched_target.emplace_back(
            level, current_search_key.GetInternalKey().ToString());
      }
      if (child_range_tombstones_[level] != nullptr) {
        child_range_tombstones_[level]->Seek(current_search_key.GetUserKey());
        // current_search_key < end_key guaranteed by the Seek() call above if
        // Valid().
        // Only interested in user key coverage since older sorted runs must
        // have smaller sequence numbers than this tombstone.
        //
        // TODO: child_range_tombstones_[level]->seq() is the max covering
        //  sequence number, can make it cheaper by not looking for max.
        if (child_range_tombstones_[level]->Valid() &&
            comparator_->user_comparator()->Compare(
                child_range_tombstones_[level]->start_key().user_key,
                current_search_key.GetUserKey()) <= 0 &&
            child_range_tombstones_[level]->seq()) {
          range_tombstone_reseek = true;
          // covered by this range tombstone
          current_search_key.SetInternalKey(
              child_range_tombstones_[level]->end_key().user_key,
              kMaxSequenceNumber);
        }
      }
    }
    // child.status() is set to Status::TryAgain indicating asynchronous
    // request for retrieval of data blocks has been submitted. So it should
    // return at this point and Seek should be called again to retrieve the
    // requested block and add the child to min heap.
    if (children_[level].status().IsTryAgain()) {
      continue;
    }
    {
      // Strictly, we timed slightly more than min heap operation,
      // but these operations are very cheap.
      PERF_TIMER_GUARD(seek_min_heap_time);
      AddToMinHeapOrCheckStatus(&children_[level]);
    }
  }
  for (size_t level = 0; level < starting_level; ++level) {
    PERF_TIMER_GUARD(seek_min_heap_time);
    AddToMinHeapOrCheckStatus(&children_[level]);
  }

  if (child_range_tombstones_.empty()) {
    for (auto& child : children_) {
      if (child.status().IsTryAgain()) {
        child.Seek(target);
        {
          PERF_TIMER_GUARD(seek_min_heap_time);
          AddToMinHeapOrCheckStatus(&child);
        }
        PERF_COUNTER_ADD(number_async_seek, 1);
      }
    }
  } else {
    for (auto& prefetch : pinned_prefetched_target) {
      children_[prefetch.first].Seek(prefetch.second);
      {
        PERF_TIMER_GUARD(seek_min_heap_time);
        AddToMinHeapOrCheckStatus(&children_[prefetch.first]);
      }
      PERF_COUNTER_ADD(number_async_seek, 1);
    }
  }
}

// Returns iff the current key (min heap top) is deleted (by some range
// deletion), advance the iterator at heap top if so. Heap order is restored.
// See FindNextVisibleEntry() for more detail on internal implementation
// of advancing child iters.
//
// REQUIRES: min heap is currently not empty, and iter is in kForward direction.
bool MergingIterator::IsNextDeleted() {
  auto current = minHeap_.top();
  auto level = GetChildIndex(current);
  ParsedInternalKey pik;
  // TODO: error handling
  ParseInternalKey(current->key(), &pik, false /* log_error_key */)
      .PermitUncheckedError();
  if (pik.type == kTypeRangeDeletion) {
    // Sentinel key: file boundary used as a fake key, always delete and move to
    // next. We need this sentinel key to keep level iterator from advancing to
    // next SST file when current range tombstone is still in effect.
    current->Next();
    if (current->Valid()) {
      minHeap_.replace_top(current);
    } else {
      considerStatus(current->status());
      minHeap_.pop();
    }
    return true /* entry deleted */;
  }

  // Check for sorted runs [0, level] for potential covering range tombstone.
  // For all sorted runs newer than the sorted run containing current key:
  //  we can advance their range tombstone iter to after current user key,
  //  since current key is at top of the heap, which means all previous
  //  iters must pointer to a user key after the current user key.
  for (size_t i = 0; i <= level; ++i) {
    // current level has no range tombstone left
    if (child_range_tombstones_[i] == nullptr ||
        !child_range_tombstones_[i]->Valid()) {
      continue;
    }

    // truncated range tombstone iter covers keys in internal key range
    if (comparator_->Compare(child_range_tombstones_[i]->end_key(), pik) <= 0) {
      // range tombstone iter is behind
      child_range_tombstones_[i]->Seek(pik.user_key);
      // Exhausted all range tombstones at i-th level
      if (!child_range_tombstones_[i]->Valid()) {
        continue;
      }
    }

    // The above successful seek guarantees current key < tombstone end key
    // (internal key), now make sure start key <= current key
    if (comparator_->Compare(pik, child_range_tombstones_[i]->start_key()) <
        0) {
      // current internal key < start internal key, no covering range tombstone
      // from this level
      continue;
    }

    // Now we know start key <= current key < end key (internal key). Check
    // sequence number if the range tombstone is from the same level as current
    // key. Note that there should be no need to seek sequence number since
    // tombstone_iter->Seek() does it and Valid() guarantees that seqno is
    // valid.
    if (i == level) {
      if (pik.sequence >= child_range_tombstones_[i]->seq()) {
        // equal case for range tombstones in ingested files: point key takes
        // precedence tombstone is older than current internal key
        continue;
      }
      // move to next key without seeking
      // Note that we could reseek all iters from older levels until the end key
      //  of the current tombstone. iters from older level will be reseeked
      //  lazily when they reach top of the queue. Since the current key will
      //  likely produce series of keys covered by the current tombstone, we
      //  need to dedup the reseek if we plan to do the reseek.
      // TODO: potentially iterate until end of tombstone before fixing the heap
      // TODO: potentially optimize this iteration by switching to seek after
      //   a certain number of iterations of the same user key.
      current->Next();
      if (current->Valid()) {
        minHeap_.replace_top(current);
      } else {
        considerStatus(current->status());
        minHeap_.pop();
      }
      return true /* entry deleted */;
    }
    assert(pik.sequence < child_range_tombstones_[i]->seq());
    // i < level
    // tombstone->Valid() means there is a valid sequence number
    std::string target;
    AppendInternalKey(&target, child_range_tombstones_[i]->end_key());
    SeekImpl(target, level, true /* tombstone_reseek */);
    return true /* entry deleted */;
  }
  return false /* not deleted */;
}

void MergingIterator::SeekForPrevImpl(const Slice& target,
                                      size_t starting_level,
                                      bool range_tombstone_reseek) {
  ClearHeaps();
  InitMaxHeap();
  status_ = Status::OK();
  IterKey current_search_key;
  current_search_key.SetInternalKey(target, false /* copy */);
  // (level, target) pairs
  autovector<std::pair<size_t, std::string>> pinned_prefetched_target;

  for (auto level = starting_level; level < children_.size(); ++level) {
    {
      PERF_TIMER_GUARD(seek_child_seek_time);
      children_[level].SeekForPrev(current_search_key.GetInternalKey());
    }

    if (range_tombstone_reseek) {
      // This seek is to some range tombstone end key
      PERF_COUNTER_ADD(internal_range_del_reseek_count, 1);
    }

    PERF_COUNTER_ADD(seek_child_seek_count, 1);

    if (!child_range_tombstones_.empty()) {
      // avoids copying target key for async requests in range tombstone free
      // path
      if (children_[level].status().IsTryAgain()) {
        // search target might change to some range tombstone end key, so
        // we need to remember them for async requests.
        pinned_prefetched_target.emplace_back(
            level, current_search_key.GetInternalKey().ToString());
      }
      if (child_range_tombstones_[level] != nullptr) {
        child_range_tombstones_[level]->SeekForPrev(
            current_search_key.GetUserKey());
        // start key <= current_search_key guaranteed by the Seek() call above
        // Only interested in user key coverage since older sorted runs must
        // have smaller sequence numbers than this tombstone.
        if (child_range_tombstones_[level]->Valid() &&
            comparator_->user_comparator()->Compare(
                current_search_key.GetUserKey(),
                child_range_tombstones_[level]->end_key().user_key) < 0 &&
            child_range_tombstones_[level]->seq()) {
          range_tombstone_reseek = true;
          // covered by this range tombstone
          current_search_key.SetInternalKey(
              child_range_tombstones_[level]->start_key().user_key,
              kMaxSequenceNumber, kValueTypeForSeekForPrev);
        }
      }
    }
    // child.status() is set to Status::TryAgain indicating asynchronous
    // request for retrieval of data blocks has been submitted. So it should
    // return at this point and Seek should be called again to retrieve the
    // requested block and add the child to min heap.
    if (children_[level].status().IsTryAgain()) {
      continue;
    }
    {
      // Strictly, we timed slightly more than min heap operation,
      // but these operations are very cheap.
      PERF_TIMER_GUARD(seek_max_heap_time);
      AddToMaxHeapOrCheckStatus(&children_[level]);
    }
  }
  for (size_t level = 0; level < starting_level; ++level) {
    PERF_TIMER_GUARD(seek_max_heap_time);
    AddToMaxHeapOrCheckStatus(&children_[level]);
  }

  if (child_range_tombstones_.empty()) {
    for (auto& child : children_) {
      if (child.status().IsTryAgain()) {
        child.Seek(target);
        {
          PERF_TIMER_GUARD(seek_min_heap_time);
          AddToMinHeapOrCheckStatus(&child);
        }
        PERF_COUNTER_ADD(number_async_seek, 1);
      }
    }
  } else {
    for (auto& prefetch : pinned_prefetched_target) {
      children_[prefetch.first].SeekForPrev(prefetch.second);
      {
        PERF_TIMER_GUARD(seek_max_heap_time);
        AddToMaxHeapOrCheckStatus(&children_[prefetch.first]);
      }
      PERF_COUNTER_ADD(number_async_seek, 1);
    }
  }
}

// Returns iff the current key (max heap top) is deleted (by some range
// deletion), move the iterator at heap top backward if so. Heap order is
// restored. See FindNextVisibleEntry() for more detail on internal
// implementation of advancing child iters.
//
// REQUIRES: max heap is currently not empty, and iter is in kReverse direction.
bool MergingIterator::IsPrevDeleted() {
  auto current = maxHeap_->top();
  auto level = GetChildIndex(current);
  ParsedInternalKey pik;
  // TODO: error handling
  ParseInternalKey(current->key(), &pik, false /* log_error_key */)
      .PermitUncheckedError();
  if (pik.type == kTypeRangeDeletion) {
    // Sentinel key: file boundary used as a fake key, always delete and move to
    // prev. We need this sentinel key to keep level iterator from advancing to
    // next SST file when current range tombstone is still in effect.
    current->Prev();
    if (current->Valid()) {
      maxHeap_->replace_top(current);
    } else {
      considerStatus(current->status());
      maxHeap_->pop();
    }
    return true /* entry deleted */;
  }

  // Check for sorted runs [0, level] for potential covering range tombstone.
  // For all sorted runs newer than the sorted run containing current key:
  //  we advance their range tombstone iter to cover current user key (or before
  //  if there is no such tombstone). We can do so since current key is at top
  //  of the heap, which means all previous iters must pointer to a user key
  //  less than or equal to the current user key.
  for (size_t i = 0; i <= level; ++i) {
    // current level has no range tombstone left
    if (child_range_tombstones_[i] == nullptr ||
        !child_range_tombstones_[i]->Valid()) {
      continue;
    }

    // truncated range tombstone iter covers keys in internal key range
    if (comparator_->Compare(pik, child_range_tombstones_[i]->start_key()) <
        0) {
      child_range_tombstones_[i]->SeekForPrev(pik.user_key);
      // Exhausted all range tombstones at i-th level
      if (!child_range_tombstones_[i]->Valid()) {
        continue;
      }
    }

    // The above successful seek guarantees tombstone start key <= current
    // internal key (internal key), now make sure current key < tombstone end
    // key
    if (comparator_->Compare(child_range_tombstones_[i]->end_key(), pik) <= 0) {
      // tombstone end key <= current key
      continue;
    }

    // Now we know start key <= current key < end key (internal key).
    // Check sequence number if the range tombstone is from the same level
    // as current key. Note that there
    // should be no need to seek sequence number since tombstone_iter->Seek()
    // does it and Valid() guarantees that seqno is valid.
    if (i == level) {
      if (pik.sequence >= child_range_tombstones_[i]->seq()) {
        // equal case for range tombstones in ingested files
        // tombstone is older than current internal key
        continue;
      }
      // move to next key without seeking
      current->Prev();
      if (current->Valid()) {
        maxHeap_->replace_top(current);
      } else {
        considerStatus(current->status());
        maxHeap_->pop();
      }
      return true /* entry deleted */;
    }
    assert(pik.sequence < child_range_tombstones_[i]->seq());
    // i < level
    // tombstone->Valid() means there is a valid sequence number
    std::string target;
    AppendInternalKey(&target, child_range_tombstones_[i]->start_key());
    // This is different from IsDeleted() which does reseek at sorted runs >=
    // level. With min heap, if level L is at top of the heap, then levels <L
    // all have internal keys > level L's current internal key,
    // which means levels <L are already at a different user key.
    // With max heap, if level L is at top of the heap, then levels <L
    // all have internal keys smaller than level L's current internal key,
    // which might still be the same user key.
    SeekForPrevImpl(target, i + 1, true /* tombstone_reseek */);
    return true /* entry deleted */;
  }
  return false /* not deleted */;
}

size_t MergingIterator::GetChildIndex(IteratorWrapper* child) {
  return child - &children_[0];
}

void MergingIterator::AddToMinHeapOrCheckStatus(IteratorWrapper* child) {
  if (child->Valid()) {
    assert(child->status().ok());
    minHeap_.push(child);
  } else {
    considerStatus(child->status());
  }
}

void MergingIterator::AddToMaxHeapOrCheckStatus(IteratorWrapper* child) {
  if (child->Valid()) {
    assert(child->status().ok());
    maxHeap_->push(child);
  } else {
    considerStatus(child->status());
  }
}

// Advance all non current_ child to > current_.key().
// We advance current_ after the this function call as it does require a Seek().
//
// Advance all range tombstones iters, including the one corresponding to
// current_, to the first tombstone with end_key > current_.key() (internal
// key).
// TODO: potentially do cascading seek here too
void MergingIterator::SwitchToForward() {
  ClearHeaps();
  Slice target = key();
  for (auto& child : children_) {
    if (&child != current_) {
      child.Seek(target);
      // child.status() is set to Status::TryAgain indicating asynchronous
      // request for retrieval of data blocks has been submitted. So it should
      // return at this point and Seek should be called again to retrieve the
      // requested block and add the child to min heap.
      if (child.status() == Status::TryAgain()) {
        continue;
      }
      if (child.Valid() && comparator_->Equal(target, child.key())) {
        assert(child.status().ok());
        child.Next();
      }
    }
    AddToMinHeapOrCheckStatus(&child);
  }

  // current range tombstone iter also need to seek for the following case:
  //
  // Previous direction is backward, so range tombstone iter may point to a
  // tombstone before current_. If there is no such tombstone, then the range
  // tombstone is !Valid(). Need to reseek here to make it valid again.
  if (!child_range_tombstones_.empty()) {
    Slice target_user_key = ExtractUserKey(target);
    for (auto& t : child_range_tombstones_) {
      if (t != nullptr) {
        t->Seek(target_user_key);
      }
    }
  }

  for (auto& child : children_) {
    if (child.status() == Status::TryAgain()) {
      child.Seek(target);
      if (child.Valid() && comparator_->Equal(target, child.key())) {
        assert(child.status().ok());
        child.Next();
      }
      AddToMinHeapOrCheckStatus(&child);
    }
  }

  direction_ = kForward;
}

void MergingIterator::SwitchToBackward() {
  ClearHeaps();
  InitMaxHeap();
  Slice target = key();
  for (auto& child : children_) {
    if (&child != current_) {
      child.SeekForPrev(target);
      TEST_SYNC_POINT_CALLBACK("MergeIterator::Prev:BeforePrev", &child);
      if (child.Valid() && comparator_->Equal(target, child.key())) {
        assert(child.status().ok());
        child.Prev();
      }
    }
    AddToMaxHeapOrCheckStatus(&child);
  }

  if (!child_range_tombstones_.empty()) {
    Slice target_user_key = ExtractUserKey(target);
    for (auto& t : child_range_tombstones_) {
      if (t != nullptr) {
        t->SeekForPrev(target_user_key);
      }
    }
  }

  direction_ = kReverse;
  if (!prefix_seek_mode_) {
    // Note that we don't do assert(current_ == CurrentReverse()) here
    // because it is possible to have some keys larger than the seek-key
    // inserted between Seek() and SeekToLast(), which makes current_ not
    // equal to CurrentReverse().
    current_ = CurrentReverse();
  }
  assert(current_ == CurrentReverse());
}

void MergingIterator::ClearHeaps() {
  minHeap_.clear();
  if (maxHeap_) {
    maxHeap_->clear();
  }
}

void MergingIterator::InitMaxHeap() {
  if (!maxHeap_) {
    maxHeap_ = std::make_unique<MergerMaxIterHeap>(comparator_);
  }
}

// For the current key (heap top), range tombstones at levels [0, current key
// level] are examined in order. If a covering tombstone is found from a
// level before current key's level, SeekImpl() is called to apply cascading
// seek from current key's level. If the covering tombstone is from current
// key's level, then the current child iterator is simply advanced to its next
// key without reseeking.
void MergingIterator::FindNextVisibleEntry() {
  while (!minHeap_.empty() && IsNextDeleted()) {
    // move to next entry
  }
}

void MergingIterator::FindPrevUserEntry() {
  while (!maxHeap_->empty() && IsPrevDeleted()) {
    // move to previous entry
  }
}

InternalIterator* NewMergingIterator(const InternalKeyComparator* cmp,
                                     InternalIterator** list, int n,
                                     Arena* arena, bool prefix_seek_mode) {
  assert(n >= 0);
  if (n == 0) {
    return NewEmptyInternalIterator<Slice>(arena);
  } else if (n == 1) {
    return list[0];
  } else {
    if (arena == nullptr) {
      return new MergingIterator(cmp, list, n, false, prefix_seek_mode);
    } else {
      auto mem = arena->AllocateAligned(sizeof(MergingIterator));
      return new (mem) MergingIterator(cmp, list, n, true, prefix_seek_mode);
    }
  }
}

MergeIteratorBuilder::MergeIteratorBuilder(
    const InternalKeyComparator* comparator, Arena* a, bool prefix_seek_mode)
    : first_iter(nullptr), use_merging_iter(false), arena(a) {
  auto mem = arena->AllocateAligned(sizeof(MergingIterator));
  merge_iter =
      new (mem) MergingIterator(comparator, nullptr, 0, true, prefix_seek_mode);
}

MergeIteratorBuilder::~MergeIteratorBuilder() {
  if (first_iter != nullptr) {
    first_iter->~InternalIterator();
  }
  if (merge_iter != nullptr) {
    merge_iter->~MergingIterator();
  }
}

void MergeIteratorBuilder::AddIterator(InternalIterator* iter) {
  if (!use_merging_iter && first_iter != nullptr) {
    merge_iter->AddIterator(first_iter);
    use_merging_iter = true;
    first_iter = nullptr;
  }
  if (use_merging_iter) {
    merge_iter->AddIterator(iter);
  } else {
    first_iter = iter;
  }
}

void MergeIteratorBuilder::AddRangeTombstoneIterator(
    TruncatedRangeDelIterator* iter,
    TruncatedRangeDelIterator*** range_del_iter_ptr) {
  if (!use_merging_iter) {
    use_merging_iter = true;
    merge_iter->AddIterator(first_iter);
    first_iter = nullptr;
  }
  merge_iter->AddRangeTombstoneIterator(iter);
  if (range_del_iter_ptr != nullptr) {
    range_del_iter_ptrs_.emplace_back(
        merge_iter->child_range_tombstones_.size() - 1, range_del_iter_ptr);
  }
}

InternalIterator* MergeIteratorBuilder::Finish(ArenaWrappedDBIter* db_iter) {
  InternalIterator* ret = nullptr;
  if (!use_merging_iter) {
    ret = first_iter;
    first_iter = nullptr;
  } else {
    for (auto& p : range_del_iter_ptrs_) {
      // Need to do this in Finish() stage instead of during
      // AddRangeTombstoneIterator() since memory address of
      // child_range_tombstones_[i] might change during vector resizing.
      *(p.second) = &(merge_iter->child_range_tombstones_[p.first]);
    }
    if (db_iter != nullptr) {
      assert(!merge_iter->child_range_tombstones_.empty());
      // memtable is always the first level
      db_iter->SetMemtableRangetombstoneIter(
          &merge_iter->child_range_tombstones_.front());
    }
    ret = merge_iter;
    merge_iter = nullptr;
  }
  return ret;
}

}  // namespace ROCKSDB_NAMESPACE
