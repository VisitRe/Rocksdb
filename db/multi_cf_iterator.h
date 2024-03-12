//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include "rocksdb/comparator.h"
#include "rocksdb/iterator.h"
#include "rocksdb/options.h"
#include "util/heap.h"

namespace ROCKSDB_NAMESPACE {

// UNDER CONSTRUCTION - DO NOT USE
// A cross-column-family iterator from a consistent database state.
// When the same key exists in more than one column families, the iterator
// selects the value from the first column family containing the key, in the
// order provided in the `column_families` parameter.
class MultiCfIterator : public Iterator {
 public:
  MultiCfIterator(const Comparator* comparator,
                  const std::vector<ColumnFamilyHandle*>& column_families,
                  const std::vector<Iterator*>& child_iterators)
      : comparator_(comparator),
        min_heap_(MultiCfMinHeapItemComparator(comparator_)),
        max_heap_(MultiCfMaxHeapItemComparator(comparator_)) {
    assert(column_families.size() > 0 &&
           column_families.size() == child_iterators.size());
    cfh_iter_pairs_.reserve(column_families.size());
    for (size_t i = 0; i < column_families.size(); ++i) {
      cfh_iter_pairs_.emplace_back(
          column_families[i], std::unique_ptr<Iterator>(child_iterators[i]));
    }
  }
  ~MultiCfIterator() override { status_.PermitUncheckedError(); }

  // No copy allowed
  MultiCfIterator(const MultiCfIterator&) = delete;
  MultiCfIterator& operator=(const MultiCfIterator&) = delete;

 private:
  std::vector<std::pair<ColumnFamilyHandle*, std::unique_ptr<Iterator>>>
      cfh_iter_pairs_;
  ReadOptions read_options_;
  Status status_;

  AttributeGroups attribute_groups_;

  struct MultiCfIteratorInfo {
    Iterator* iterator;
    ColumnFamilyHandle* cfh;
    int order;
  };

  class MultiCfMinHeapItemComparator {
   public:
    explicit MultiCfMinHeapItemComparator(const Comparator* comparator)
        : comparator_(comparator) {}

    bool operator()(const MultiCfIteratorInfo& a,
                    const MultiCfIteratorInfo& b) const {
      assert(a.iterator);
      assert(b.iterator);
      assert(a.iterator->Valid());
      assert(b.iterator->Valid());
      int c = comparator_->Compare(a.iterator->key(), b.iterator->key());
      assert(c != 0 || a.order != b.order);
      return c == 0 ? a.order - b.order > 0 : c > 0;
    }

   private:
    const Comparator* comparator_;
  };

  class MultiCfMaxHeapItemComparator {
   public:
    explicit MultiCfMaxHeapItemComparator(const Comparator* comparator)
        : comparator_(comparator) {}

    bool operator()(const MultiCfIteratorInfo& a,
                    const MultiCfIteratorInfo& b) const {
      assert(a.iterator);
      assert(b.iterator);
      assert(a.iterator->Valid());
      assert(b.iterator->Valid());
      int c = comparator_->Compare(a.iterator->key(), b.iterator->key());
      assert(c != 0 || a.order != b.order);
      return c == 0 ? a.order - b.order > 0 : c < 0;
    }

   private:
    const Comparator* comparator_;
  };

  const Comparator* comparator_;
  using MultiCfMinHeap =
      BinaryHeap<MultiCfIteratorInfo, MultiCfMinHeapItemComparator>;
  using MultiCfMaxHeap =
      BinaryHeap<MultiCfIteratorInfo, MultiCfMaxHeapItemComparator>;
  MultiCfMinHeap min_heap_;
  MultiCfMaxHeap max_heap_;

  enum Direction : uint8_t { kForward, kReverse };
  Direction direction_ = kForward;

  // TODO: Lower and Upper bounds

  Iterator* current() const {
    if (direction_ == kReverse) {
      return max_heap_.top().iterator;
    }
    return min_heap_.top().iterator;
  }

  Slice key() const override {
    assert(Valid());
    return current()->key();
  }
  Slice value() const override {
    assert(Valid());
    return current()->value();
  }
  const WideColumns& columns() const override {
    assert(Valid());
    return current()->columns();
  }

  bool Valid() const override {
    if (direction_ == kReverse) {
      return !max_heap_.empty() && status_.ok();
    }
    return !min_heap_.empty() && status_.ok();
  }
  Status status() const override { return status_; }
  void considerStatus(Status s) {
    if (!s.ok() && status_.ok()) {
      status_ = std::move(s);
    }
  }
  void Reset() {
    min_heap_.clear();
    max_heap_.clear();
    status_ = Status::OK();
  }

  void SwitchToDirection(Direction new_direction) {
    assert(direction_ != new_direction);
    Slice target = key();
    if (new_direction == kForward) {
      Seek(target);
    } else {
      SeekForPrev(target);
    }
  }

  void SeekCommon(const std::function<void(Iterator*)>& child_seek_func,
                  Direction direction);
  template <typename BinaryHeap>
  void AdvanceIterator(BinaryHeap& heap,
                       const std::function<void(Iterator*)>& advance_func);

  void SeekToFirst() override;
  void SeekToLast() override;
  void Seek(const Slice& /*target*/) override;
  void SeekForPrev(const Slice& /*target*/) override;
  void Next() override;
  void Prev() override;
};

}  // namespace ROCKSDB_NAMESPACE
