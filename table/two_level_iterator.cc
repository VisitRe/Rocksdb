//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/two_level_iterator.h"
#include "db/pinned_iterators_manager.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"
#include "table/block.h"
#include "table/format.h"
#include "util/arena.h"

namespace rocksdb {

namespace {

class TwoLevelIterator : public InternalIterator {
 public:
  explicit TwoLevelIterator(TwoLevelIteratorState* state,
                            InternalIterator* first_level_iter);

  virtual ~TwoLevelIterator() {
    delete first_level_iter_;
    delete second_level_iter_;
    delete state_;
  }

  virtual void Seek(const Slice& target) override;
  virtual void SeekForPrev(const Slice& target) override;
  virtual void SeekToFirst() override;
  virtual void SeekToLast() override;
  virtual void Next() override;
  virtual void Prev() override;

  virtual bool Valid() const override { return second_level_iter_->Valid(); }
  virtual Slice key() const override {
    assert(Valid());
    return second_level_iter_->key();
  }

  ParsedInternalKey parsed_internal_key() const override {
    assert(Valid());
    return second_level_iter_->parsed_internal_key();
  }

  virtual Slice value() const override {
    assert(Valid());
    return second_level_iter_->value();
  }
  virtual Status status() const override {
    if (!first_level_iter_->status().ok()) {
      assert(second_level_iter_ == nullptr);
      return first_level_iter_->status();
    } else if (second_level_iter_ != nullptr &&
               !second_level_iter_->status().ok()) {
      return second_level_iter_->status();
    } else {
      return status_;
    }
  }
  virtual void SetPinnedItersMgr(
      PinnedIteratorsManager* /*pinned_iters_mgr*/) override {}
  virtual bool IsKeyPinned() const override { return false; }
  virtual bool IsValuePinned() const override { return false; }

 private:
  void SaveError(const Status& s) {
    if (status_.ok() && !s.ok()) status_ = s;
  }
  void SkipEmptyDataBlocksForward();
  void SkipEmptyDataBlocksBackward();
  void SetSecondLevelIterator(InternalIterator* iter);
  void InitDataBlock();

  TwoLevelIteratorState* state_;
  InternalIterator* first_level_iter_;
  InternalIterator* second_level_iter_ = nullptr;
  Status status_;
  // If second_level_iter is non-nullptr, then "data_block_handle_" holds the
  // "index_value" passed to block_function_ to create the second_level_iter.
  std::string data_block_handle_;
};

TwoLevelIterator::TwoLevelIterator(TwoLevelIteratorState* state,
                                   InternalIterator* first_level_iter)
    : state_(state), first_level_iter_(first_level_iter) {}

void TwoLevelIterator::Seek(const Slice& target) {
  first_level_iter_->Seek(target);

  InitDataBlock();
  if (second_level_iter_ != nullptr) {
    second_level_iter_->Seek(target);
  }
  SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::SeekForPrev(const Slice& target) {
  first_level_iter_->Seek(target);
  InitDataBlock();
  if (second_level_iter_ != nullptr) {
    second_level_iter_->SeekForPrev(target);
  }
  if (!Valid()) {
    if (!first_level_iter_->Valid() && first_level_iter_->status().ok()) {
      first_level_iter_->SeekToLast();
      InitDataBlock();
      if (second_level_iter_ != nullptr) {
        second_level_iter_->SeekForPrev(target);
      }
    }
    SkipEmptyDataBlocksBackward();
  }
}

void TwoLevelIterator::SeekToFirst() {
  first_level_iter_->SeekToFirst();
  InitDataBlock();
  if (second_level_iter_ != nullptr) {
    second_level_iter_->SeekToFirst();
  }
  SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::SeekToLast() {
  first_level_iter_->SeekToLast();
  InitDataBlock();
  if (second_level_iter_ != nullptr) {
    second_level_iter_->SeekToLast();
  }
  SkipEmptyDataBlocksBackward();
}

void TwoLevelIterator::Next() {
  assert(Valid());
  second_level_iter_->Next();
  SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::Prev() {
  assert(Valid());
  second_level_iter_->Prev();
  SkipEmptyDataBlocksBackward();
}

void TwoLevelIterator::SkipEmptyDataBlocksForward() {
  while (second_level_iter_ == nullptr ||
         (!second_level_iter_->Valid() && second_level_iter_->status().ok())) {
    // Move to next block
    if (!first_level_iter_->Valid()) {
      SetSecondLevelIterator(nullptr);
      return;
    }
    first_level_iter_->Next();
    InitDataBlock();
    if (second_level_iter_ != nullptr) {
      second_level_iter_->SeekToFirst();
    }
  }
}

void TwoLevelIterator::SkipEmptyDataBlocksBackward() {
  while (second_level_iter_ == nullptr ||
         (!second_level_iter_->Valid() && second_level_iter_->status().ok())) {
    // Move to next block
    if (!first_level_iter_->Valid()) {
      SetSecondLevelIterator(nullptr);
      return;
    }
    first_level_iter_->Prev();
    InitDataBlock();
    if (second_level_iter_ != nullptr) {
      second_level_iter_->SeekToLast();
    }
  }
}

void TwoLevelIterator::SetSecondLevelIterator(InternalIterator* iter) {
  delete second_level_iter_;
  second_level_iter_ = iter;
}

void TwoLevelIterator::InitDataBlock() {
  if (!first_level_iter_->Valid()) {
    SetSecondLevelIterator(nullptr);
  } else {
    Slice handle = first_level_iter_->value();
    if (second_level_iter_ != nullptr &&
        !second_level_iter_->status().IsIncomplete() &&
        handle.compare(data_block_handle_) == 0) {
      // second_level_iter is already constructed with this iterator, so
      // no need to change anything
    } else {
      InternalIterator* iter = state_->NewSecondaryIterator(handle);
      data_block_handle_.assign(handle.data(), handle.size());
      SetSecondLevelIterator(iter);
    }
  }
}

}  // namespace

InternalIterator* NewTwoLevelIterator(TwoLevelIteratorState* state,
                                      InternalIterator* first_level_iter) {
  return new TwoLevelIterator(state, first_level_iter);
}
}  // namespace rocksdb
