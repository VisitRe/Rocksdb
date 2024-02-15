//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include "rocksdb/multi_cf_iterator.h"
#include "util/heap.h"

namespace ROCKSDB_NAMESPACE {

// UNDER CONSTRUCTION - DO NOT USE
// A cross-column-family iterator from a consistent database state.
// When the same key exists in more than one column families, the iterator
// selects the value from the first column family containing the key, in the
// order provided in the `column_families` parameter.
class MultiCfIteratorImpl : public MultiCfIterator {
 public:
  MultiCfIteratorImpl() {}
  MultiCfIteratorImpl(const Comparator* comparator,
                      const std::vector<ColumnFamilyHandle*>& column_families,
                      const std::vector<Iterator*>& child_iterators)
      : comparator_(comparator),
        min_heap_(MultiCfMinHeapItemComparator(comparator_)) {
    assert(column_families.size() > 0);
    assert(column_families.size() == child_iterators.size());

    cfhs_.reserve(column_families.size());
    iterators_.reserve(column_families.size());
    for (size_t i = 0; i < column_families.size(); ++i) {
      cfhs_.push_back(column_families[i]);
      iterators_.push_back(child_iterators[i]);
    }
  }
  ~MultiCfIteratorImpl() override {
    for (auto iter : iterators_) {
      delete iter;
    }
    status_.PermitUncheckedError();
  }

  // No copy allowed
  MultiCfIteratorImpl(const MultiCfIteratorImpl&) = delete;
  MultiCfIteratorImpl& operator=(const MultiCfIteratorImpl&) = delete;

 private:
  std::vector<ColumnFamilyHandle*> cfhs_;
  std::vector<Iterator*> iterators_;
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
    MultiCfMinHeapItemComparator() {}
    explicit MultiCfMinHeapItemComparator(const Comparator* comparator)
        : comparator_(comparator) {}

    bool operator()(MultiCfIteratorInfo a, MultiCfIteratorInfo b) const {
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

  const Comparator* comparator_;
  using MultiCfMinHeap =
      BinaryHeap<MultiCfIteratorInfo, MultiCfMinHeapItemComparator>;
  MultiCfMinHeap min_heap_;
  // TODO: MaxHeap for Reverse Iteration
  // TODO: Lower and Upper bounds

  Slice key() const override {
    assert(Valid());
    return min_heap_.top().iterator->key();
  }
  bool Valid() const override { return !min_heap_.empty() && status_.ok(); }
  Status status() const override { return status_; }
  void considerStatus(Status s) {
    if (!s.ok() && status_.ok()) {
      status_ = std::move(s);
    }
  }
  void Reset() {
    min_heap_.clear();
    status_ = Status::OK();
  }

  void SeekToFirst() override;
  void Next() override;

  // TODO - Implement these
  void Seek(const Slice& /*target*/) override {}
  void SeekForPrev(const Slice& /*target*/) override {}
  void SeekToLast() override {}
  void Prev() override { assert(false); }
  Slice value() const override {
    assert(Valid());
    return min_heap_.top().iterator->value();
  }
  const WideColumns& columns() const override {
    assert(Valid());
    return min_heap_.top().iterator->columns();
  }
  const AttributeGroups& attribute_groups() const override {
    assert(false);
    // TODO - Lazily populate attribute groups from child iterators
    return attribute_groups_;
  }
};

class EmptyMultiCfIterator : public MultiCfIterator {
 public:
  explicit EmptyMultiCfIterator(const Status& s) : status_(s) {}
  bool Valid() const override { return false; }
  void Seek(const Slice& /*target*/) override {}
  void SeekForPrev(const Slice& /*target*/) override {}
  void SeekToFirst() override {}
  void SeekToLast() override {}
  void Next() override { assert(false); }
  void Prev() override { assert(false); }
  Slice key() const override {
    assert(false);
    return Slice();
  }
  Slice value() const override {
    assert(false);
    return Slice();
  }
  const AttributeGroups& attribute_groups() const override {
    assert(false);
    return kNoAttributeGroups;
  }
  Status status() const override { return status_; }

 private:
  Status status_;
};

}  // namespace ROCKSDB_NAMESPACE