//  Copyright (c) 2018-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <list>
#include <memory>
#include <string>
#include <vector>

#include "db/dbformat.h"
#include "db/pinned_iterators_manager.h"
#include "rocksdb/status.h"
#include "table/internal_iterator.h"

namespace rocksdb {

struct FragmentedRangeTombstoneList {
 public:
  // A compact representation of a "stack" of range tombstone fragments, which
  // start and end at the same user keys but have different sequence numbers.
  // The members seq_start_idx and seq_end_idx are intended to be parameters to
  // seq_iter().
  struct RangeTombstoneStack {
    RangeTombstoneStack(const Slice& start, const Slice& end, int start_idx,
                        int end_idx)
        : start_key(start),
          end_key(end),
          seq_start_idx(start_idx),
          seq_end_idx(end_idx) {}

    Slice start_key;
    Slice end_key;
    size_t seq_start_idx;
    // TODO: see if seq_end_idx can be removed
    size_t seq_end_idx;
  };
  FragmentedRangeTombstoneList(
      std::unique_ptr<InternalIterator> unfragmented_tombstones,
      const InternalKeyComparator& icmp, bool one_time_use,
      SequenceNumber snapshot = kMaxSequenceNumber);

  std::vector<RangeTombstoneStack>::const_iterator begin() const {
    return tombstones_.begin();
  }

  std::vector<RangeTombstoneStack>::const_iterator end() const {
    return tombstones_.end();
  }

  std::vector<SequenceNumber>::const_iterator seq_iter(size_t idx) const {
    return std::next(tombstone_seqs_.begin(), idx);
  }

  std::vector<SequenceNumber>::const_iterator seq_begin() const {
    return tombstone_seqs_.begin();
  }

  std::vector<SequenceNumber>::const_iterator seq_end() const {
    return tombstone_seqs_.end();
  }

  bool empty() const { return tombstones_.size() == 0; }

 private:
  // Given an ordered range tombstone iterator unfragmented_tombstones,
  // "fragment" the tombstones into non-overlapping pieces, and store them in
  // tombstones_ and tombstone_seqs_.
  void FragmentTombstones(
      std::unique_ptr<InternalIterator> unfragmented_tombstones,
      const InternalKeyComparator& icmp, bool one_time_use,
      SequenceNumber snapshot = kMaxSequenceNumber);

  std::vector<RangeTombstoneStack> tombstones_;
  std::vector<SequenceNumber> tombstone_seqs_;
  std::list<std::string> pinned_slices_;
  PinnedIteratorsManager pinned_iters_mgr_;
};

// FragmentedRangeTombstoneIterator converts an InternalIterator of a range-del
// meta block into an iterator over non-overlapping tombstone fragments. The
// tombstone fragmentation process should be more efficient than the range
// tombstone collapsing algorithm in RangeDelAggregator because this leverages
// the internal key ordering already provided by the input iterator, if
// applicable (when the iterator is unsorted, a new sorted iterator is created
// before proceeding). If there are few overlaps, creating a
// FragmentedRangeTombstoneIterator should be O(n), while the RangeDelAggregator
// tombstone collapsing is always O(n log n).
class FragmentedRangeTombstoneIterator : public InternalIterator {
 public:
  FragmentedRangeTombstoneIterator(
      const FragmentedRangeTombstoneList* tombstones,
      const InternalKeyComparator& icmp);
  FragmentedRangeTombstoneIterator(
      const std::shared_ptr<const FragmentedRangeTombstoneList>& tombstones,
      const InternalKeyComparator& icmp);
  void SeekToFirst() override;
  void SeekToLast() override;

  // Seeks to the range tombstone that covers target's user key at a seqnum
  // at most target's seqnum. If no such tombstone exists, seek to the earliest
  // tombstone that ends after target (regardless of its seqnum).
  void Seek(const Slice& target) override;
  // Seeks to the range tombstone that covers target's user key at a seqnum
  // at most target's seqnum. If no such tombstone exists, seek to the latest
  // tombstone that starts before target (regardless of its seqnum).
  void SeekForPrev(const Slice& target) override;

  void Next() override;
  void Prev() override;
  bool Valid() const override;
  Slice key() const override {
    MaybePinKey();
    return current_start_key_.Encode();
  }
  Slice value() const override { return pos_->end_key; }
  bool IsKeyPinned() const override { return false; }
  bool IsValuePinned() const override { return true; }
  Status status() const override { return Status::OK(); }

  Slice start_key() const { return pos_->start_key; }
  Slice end_key() const { return pos_->end_key; }
  SequenceNumber seq() const { return *seq_pos_; }

 private:
  using RangeTombstoneStack = FragmentedRangeTombstoneList::RangeTombstoneStack;

  struct RangeTombstoneStackStartComparator {
    explicit RangeTombstoneStackStartComparator(const Comparator* c) : cmp(c) {}

    bool operator()(const RangeTombstoneStack& a,
                    const RangeTombstoneStack& b) const {
      return cmp->Compare(a.start_key, b.start_key) < 0;
    }

    bool operator()(const RangeTombstoneStack& a, const Slice& b) const {
      return cmp->Compare(a.start_key, b) < 0;
    }

    bool operator()(const Slice& a, const RangeTombstoneStack& b) const {
      return cmp->Compare(a, b.start_key) < 0;
    }

    const Comparator* cmp;
  };

  struct RangeTombstoneStackEndComparator {
    explicit RangeTombstoneStackEndComparator(const Comparator* c) : cmp(c) {}

    bool operator()(const RangeTombstoneStack& a,
                    const RangeTombstoneStack& b) const {
      return cmp->Compare(a.end_key, b.end_key) < 0;
    }

    bool operator()(const RangeTombstoneStack& a, const Slice& b) const {
      return cmp->Compare(a.end_key, b) < 0;
    }

    bool operator()(const Slice& a, const RangeTombstoneStack& b) const {
      return cmp->Compare(a, b.end_key) < 0;
    }

    const Comparator* cmp;
  };

  void MaybePinKey() const {
    if (pos_ != tombstones_->end() && pinned_pos_ != pos_ &&
        seq_pos_ != tombstones_->seq_end() && pinned_seq_pos_ != seq_pos_) {
      current_start_key_.Set(pos_->start_key, *seq_pos_, kTypeRangeDeletion);
      pinned_pos_ = pos_;
      pinned_seq_pos_ = seq_pos_;
    }
  }

  const RangeTombstoneStackStartComparator tombstone_start_cmp_;
  const RangeTombstoneStackEndComparator tombstone_end_cmp_;
  const Comparator* ucmp_;
  std::shared_ptr<const FragmentedRangeTombstoneList> tombstones_ref_;
  const FragmentedRangeTombstoneList* tombstones_;
  std::vector<RangeTombstoneStack>::const_iterator pos_;
  std::vector<SequenceNumber>::const_iterator seq_pos_;
  mutable std::vector<RangeTombstoneStack>::const_iterator pinned_pos_;
  mutable std::vector<SequenceNumber>::const_iterator pinned_seq_pos_;
  mutable InternalKey current_start_key_;
  PinnedIteratorsManager pinned_iters_mgr_;
};

SequenceNumber MaxCoveringTombstoneSeqnum(
    FragmentedRangeTombstoneIterator* tombstone_iter, const Slice& key,
    const Comparator* ucmp);

}  // namespace rocksdb
