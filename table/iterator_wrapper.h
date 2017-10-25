//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once

#include <set>

#include "table/internal_iterator.h"

namespace rocksdb {

// A internal wrapper class with an interface similar to Iterator that caches
// the valid() and key() results for an underlying iterator.
// This can help avoid virtual function calls and also gives better
// cache locality.
class IteratorWrapper {
 public:
  using Callback = InternalIterator::Callback;
  IteratorWrapper() : iter_(nullptr), valid_(false) {}
  explicit IteratorWrapper(InternalIterator* _iter) : iter_(nullptr) {
    Set(_iter);
  }
  ~IteratorWrapper() {}
  InternalIterator* iter() const { return iter_; }

  // Set the underlying Iterator to _iter and return
  // previous underlying Iterator.
  InternalIterator* Set(InternalIterator* _iter) {
    InternalIterator* old_iter = iter_;

    iter_ = _iter;
    if (iter_ == nullptr) {
      valid_ = false;
    } else {
      Update();
    }
    return old_iter;
  }

  void DeleteIter(bool is_arena_mode) {
    if (iter_) {
      if (!is_arena_mode) {
        delete iter_;
      } else {
        iter_->~InternalIterator();
      }
    }
  }

  // Iterator interface methods
  bool Valid() const        { return valid_; }
  Slice key() const         { assert(Valid()); return key_; }
  Slice value() const       { assert(Valid()); return iter_->value(); }
  // Methods below require iter() != nullptr
  Status status() const     { assert(iter_); return iter_->status(); }
  void Next()               { assert(iter_); iter_->Next();        Update(); }
  void Prev()               { assert(iter_); iter_->Prev();        Update(); }
  void Seek(const Slice& k) { assert(iter_); iter_->Seek(k);       Update(); }
  void SeekForPrev(const Slice& k) {
    assert(iter_);
    iter_->SeekForPrev(k);
    Update();
  }
  void SeekToFirst()        { assert(iter_); iter_->SeekToFirst(); Update(); }
  void SeekToLast()         { assert(iter_); iter_->SeekToLast();  Update(); }

  // All Async overloads require Update() follow up call on async return
  // i.e. when the interface returns IOPending status
  Status RequestNext(const Callback& cb) {
    assert(iter_); 
    Status s = iter_->RequestNext(cb);
    if (!s.IsIOPending()) {
      Update();
    }
    return s;
  }

  Status RequestPrev(const Callback& cb) {
    assert(iter_); 
    Status s = iter_->RequestPrev(cb);
    if (!s.IsIOPending()) {
      Update();
    }
    return s;
  }

  Status RequestSeek(const Callback& cb, const Slice& k) {
    assert(iter_); 
    Status s = iter_->RequestSeek(cb, k);
    if (!s.IsIOPending()) {
      Update();
    }
    return s;
  }
  Status RequestSeekForPrev(const Callback& cb, const Slice& k) {
    assert(iter_);
    Status s = iter_->RequestSeekForPrev(cb, k);
    if (!s.IsIOPending()) {
      Update();
    }
    return s;
  }
  Status RequestSeekToFirst(const Callback& cb) {
    assert(iter_);
    Status s = iter_->RequestSeekToFirst(cb);
    if (!s.IsIOPending()) {
      Update();
    }
    return s;
  }
  Status RequestSeekToLast(const Callback& cb) {
    assert(iter_); 
    Status s = iter_->RequestSeekToLast(cb);
    if (!s.IsIOPending()) {
      Update();
    }
    return s;
  }

  void SetPinnedItersMgr(PinnedIteratorsManager* pinned_iters_mgr) {
    assert(iter_);
    iter_->SetPinnedItersMgr(pinned_iters_mgr);
  }
  bool IsKeyPinned() const {
    assert(Valid());
    return iter_->IsKeyPinned();
  }
  bool IsValuePinned() const {
    assert(Valid());
    return iter_->IsValuePinned();
  }

 private:

  friend class MergingIteratorAsync;
  friend class TwoLevelIteratorBlockBasedAsync;
  void Update() {
    valid_ = iter_->Valid();
    if (valid_) {
      key_ = iter_->key();
    }
  }

  InternalIterator* iter_;
  bool valid_;
  Slice key_;
};

class Arena;
// Return an empty iterator (yields nothing) allocated from arena.
extern InternalIterator* NewEmptyInternalIterator(Arena* arena);

// Return an empty iterator with the specified status, allocated arena.
extern InternalIterator* NewErrorInternalIterator(const Status& status,
                                                  Arena* arena);

}  // namespace rocksdb
