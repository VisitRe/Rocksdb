//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Decodes the blocks generated by block_builder.cc.

#include "table/block.h"
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "monitoring/perf_context_imp.h"
#include "port/port.h"
#include "port/stack_trace.h"
#include "rocksdb/comparator.h"
#include "table/block_prefix_index.h"
#include "table/format.h"
#include "util/coding.h"
#include "util/logging.h"

namespace rocksdb {

// Helper routine: decode the next block entry starting at "p",
// storing the number of shared key bytes, non_shared key bytes,
// and the length of the value in "*shared", "*non_shared", and
// "*value_length", respectively.  Will not derefence past "limit".
//
// If any errors are detected, returns nullptr.  Otherwise, returns a
// pointer to the key delta (just past the three decoded values).
static inline const char* DecodeEntry(const char* p, const char* limit,
                                      uint32_t* shared,
                                      uint32_t* non_shared,
                                      uint32_t* value_length) {
  if (limit - p < 3) return nullptr;
  *shared = reinterpret_cast<const unsigned char*>(p)[0];
  *non_shared = reinterpret_cast<const unsigned char*>(p)[1];
  *value_length = reinterpret_cast<const unsigned char*>(p)[2];
  if ((*shared | *non_shared | *value_length) < 128) {
    // Fast path: all three values are encoded in one byte each
    p += 3;
  } else {
    if ((p = GetVarint32Ptr(p, limit, shared)) == nullptr) return nullptr;
    if ((p = GetVarint32Ptr(p, limit, non_shared)) == nullptr) return nullptr;
    if ((p = GetVarint32Ptr(p, limit, value_length)) == nullptr) return nullptr;
  }

  if (static_cast<uint32_t>(limit - p) < (*non_shared + *value_length)) {
    return nullptr;
  }
  return p;
}

void DataBlockIter::Next() {
  assert(Valid());
  ParseNextDataKey();
}

void IndexBlockIter::Next() {
  assert(Valid());
  ParseNextIndexKey();
}

void IndexBlockIter::Prev() {
  assert(Valid());
  // Scan backwards to a restart point before current_
  const uint32_t original = current_;
  while (GetRestartPoint(restart_index_) >= original) {
    if (restart_index_ == 0) {
      // No more entries
      current_ = restarts_;
      restart_index_ = num_restarts_;
      return;
    }
    restart_index_--;
  }
  SeekToRestartPoint(restart_index_);
  do {
    if (!ParseNextIndexKey()) {
      break;
    }
    // Loop until end of current entry hits the start of original entry
  } while (NextEntryOffset() < original);
}

// Similar to IndexBlockIter::Prev but also caches the prev entries
void DataBlockIter::Prev() {
  assert(Valid());

  assert(prev_entries_idx_ == -1 ||
         static_cast<size_t>(prev_entries_idx_) < prev_entries_.size());
  // Check if we can use cached prev_entries_
  if (prev_entries_idx_ > 0 &&
      prev_entries_[prev_entries_idx_].offset == current_) {
    // Read cached CachedPrevEntry
    prev_entries_idx_--;
    const CachedPrevEntry& current_prev_entry =
        prev_entries_[prev_entries_idx_];

    const char* key_ptr = nullptr;
    if (current_prev_entry.key_ptr != nullptr) {
      // The key is not delta encoded and stored in the data block
      key_ptr = current_prev_entry.key_ptr;
      key_pinned_ = true;
    } else {
      // The key is delta encoded and stored in prev_entries_keys_buff_
      key_ptr = prev_entries_keys_buff_.data() + current_prev_entry.key_offset;
      key_pinned_ = false;
    }
    const Slice current_key(key_ptr, current_prev_entry.key_size);

    current_ = current_prev_entry.offset;
    key_.SetKey(current_key, false /* copy */);
    value_ = current_prev_entry.value;

    return;
  }

  // Clear prev entries cache
  prev_entries_idx_ = -1;
  prev_entries_.clear();
  prev_entries_keys_buff_.clear();

  // Scan backwards to a restart point before current_
  const uint32_t original = current_;
  while (GetRestartPoint(restart_index_) >= original) {
    if (restart_index_ == 0) {
      // No more entries
      current_ = restarts_;
      restart_index_ = num_restarts_;
      return;
    }
    restart_index_--;
  }

  SeekToRestartPoint(restart_index_);

  do {
    if (!ParseNextDataKey()) {
      break;
    }
    Slice current_key = key();

    if (key_.IsKeyPinned()) {
      // The key is not delta encoded
      prev_entries_.emplace_back(current_, current_key.data(), 0,
                                 current_key.size(), value());
    } else {
      // The key is delta encoded, cache decoded key in buffer
      size_t new_key_offset = prev_entries_keys_buff_.size();
      prev_entries_keys_buff_.append(current_key.data(), current_key.size());

      prev_entries_.emplace_back(current_, nullptr, new_key_offset,
                                 current_key.size(), value());
    }
    // Loop until end of current entry hits the start of original entry
  } while (NextEntryOffset() < original);
  prev_entries_idx_ = static_cast<int32_t>(prev_entries_.size()) - 1;
}

void DataBlockIter::Seek(const Slice& target) {
  Slice seek_key = target;
  PERF_TIMER_GUARD(block_seek_nanos);
  if (data_ == nullptr) {  // Not init yet
    return;
  }
  uint32_t index = 0;

  if (data_block_hash_index_) {
    // suffix seek will set the current_ and restart_index_,
    // no need to pass back `index`, or linear search.
    HashSeek(target);
    return;
  }

  bool ok = BinarySeek(seek_key, 0, num_restarts_ - 1, &index, comparator_);

  if (!ok) {
    return;
  }
  SeekToRestartPoint(index);
  // Linear search (within restart block) for first key >= target

  while (true) {
    if (!ParseNextDataKey() || Compare(key_, seek_key) >= 0) {
      return;
    }
  }
}

void IndexBlockIter::Seek(const Slice& target) {
  Slice seek_key = target;
  if (!key_includes_seq_) {
    seek_key = ExtractUserKey(target);
  }
  PERF_TIMER_GUARD(block_seek_nanos);
  if (data_ == nullptr) {  // Not init yet
    return;
  }
  uint32_t index = 0;
  bool ok = false;
  if (prefix_index_) {
    ok = PrefixSeek(target, &index);
  } else {
    ok = BinarySeek(seek_key, 0, num_restarts_ - 1, &index, active_comparator_);
  }

  if (!ok) {
    return;
  }
  SeekToRestartPoint(index);
  // Linear search (within restart block) for first key >= target

  while (true) {
    if (!ParseNextIndexKey() || Compare(key_, seek_key) >= 0) {
      return;
    }
  }
}

void DataBlockIter::SeekForPrev(const Slice& target) {
  PERF_TIMER_GUARD(block_seek_nanos);
  Slice seek_key = target;
  if (data_ == nullptr) {  // Not init yet
    return;
  }
  uint32_t index = 0;
  bool ok = BinarySeek(seek_key, 0, num_restarts_ - 1, &index, comparator_);

  if (!ok) {
    return;
  }
  SeekToRestartPoint(index);
  // Linear search (within restart block) for first key >= seek_key

  while (ParseNextDataKey() && Compare(key_, seek_key) < 0) {
  }
  if (!Valid()) {
    SeekToLast();
  } else {
    while (Valid() && Compare(key_, seek_key) > 0) {
      Prev();
    }
  }
}

void DataBlockIter::SeekToFirst() {
  if (data_ == nullptr) {  // Not init yet
    return;
  }
  SeekToRestartPoint(0);
  ParseNextDataKey();
}

void IndexBlockIter::SeekToFirst() {
  if (data_ == nullptr) {  // Not init yet
    return;
  }
  SeekToRestartPoint(0);
  ParseNextIndexKey();
}

void DataBlockIter::SeekToLast() {
  if (data_ == nullptr) {  // Not init yet
    return;
  }
  SeekToRestartPoint(num_restarts_ - 1);
  while (ParseNextDataKey() && NextEntryOffset() < restarts_) {
    // Keep skipping
  }
}

void IndexBlockIter::SeekToLast() {
  if (data_ == nullptr) {  // Not init yet
    return;
  }
  SeekToRestartPoint(num_restarts_ - 1);
  while (ParseNextIndexKey() && NextEntryOffset() < restarts_) {
    // Keep skipping
  }
}

void BlockIter::CorruptionError() {
  current_ = restarts_;
  restart_index_ = num_restarts_;
  status_ = Status::Corruption("bad entry in block");
  key_.Clear();
  value_.clear();
}

// if within_restart_interval == true, we only parse next key within
// current restart invterval. The default value of within_restart_interval
// is false.
bool DataBlockIter::ParseNextDataKey(bool within_restart_interval) {
  current_ = NextEntryOffset();
  const char* p = data_ + current_;
  const char* limit = data_ + restarts_;  // Restarts come right after data
  if (within_restart_interval) {
    assert(restart_index_ < num_restarts_);
    if (restart_index_ + 1 < num_restarts_) {
      limit = data_ + GetRestartPoint(restart_index_ + 1);
    }
  }

  if (p >= limit) {
    // No more entries to return.  Mark as invalid.
    current_ = restarts_;
    restart_index_ = num_restarts_;
    return false;
  }

  // Decode next entry
  uint32_t shared, non_shared, value_length;
  p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
  if (p == nullptr || key_.Size() < shared) {
    CorruptionError();
    return false;
  } else {
    if (shared == 0) {
      // If this key dont share any bytes with prev key then we dont need
      // to decode it and can use it's address in the block directly.
      key_.SetKey(Slice(p, non_shared), false /* copy */);
      key_pinned_ = true;
    } else {
      // This key share `shared` bytes with prev key, we need to decode it
      key_.TrimAppend(shared, p, non_shared);
      key_pinned_ = false;
    }

    if (global_seqno_ != kDisableGlobalSequenceNumber) {
      // If we are reading a file with a global sequence number we should
      // expect that all encoded sequence numbers are zeros and any value
      // type is kTypeValue, kTypeMerge, kTypeDeletion, or kTypeRangeDeletion.
      assert(GetInternalKeySeqno(key_.GetInternalKey()) == 0);

      ValueType value_type = ExtractValueType(key_.GetKey());
      assert(value_type == ValueType::kTypeValue ||
             value_type == ValueType::kTypeMerge ||
             value_type == ValueType::kTypeDeletion ||
             value_type == ValueType::kTypeRangeDeletion);

      if (key_pinned_) {
        // TODO(tec): Investigate updating the seqno in the loaded block
        // directly instead of doing a copy and update.

        // We cannot use the key address in the block directly because
        // we have a global_seqno_ that will overwrite the encoded one.
        key_.OwnKey();
        key_pinned_ = false;
      }

      key_.UpdateInternalKey(global_seqno_, value_type);
    }

    value_ = Slice(p + non_shared, value_length);
    while (restart_index_ + 1 < num_restarts_ &&
           GetRestartPoint(restart_index_ + 1) < current_) {
      ++restart_index_;
    }
    return true;
  }
}

bool IndexBlockIter::ParseNextIndexKey() {
  current_ = NextEntryOffset();
  const char* p = data_ + current_;
  const char* limit = data_ + restarts_;  // Restarts come right after data
  if (p >= limit) {
    // No more entries to return.  Mark as invalid.
    current_ = restarts_;
    restart_index_ = num_restarts_;
    return false;
  }

  // Decode next entry
  uint32_t shared, non_shared, value_length;
  p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
  if (p == nullptr || key_.Size() < shared) {
    CorruptionError();
    return false;
  }
  if (shared == 0) {
    // If this key dont share any bytes with prev key then we dont need
    // to decode it and can use it's address in the block directly.
    key_.SetKey(Slice(p, non_shared), false /* copy */);
    key_pinned_ = true;
  } else {
    // This key share `shared` bytes with prev key, we need to decode it
    key_.TrimAppend(shared, p, non_shared);
    key_pinned_ = false;
  }
  value_ = Slice(p + non_shared, value_length);
  while (restart_index_ + 1 < num_restarts_ &&
         GetRestartPoint(restart_index_ + 1) < current_) {
    ++restart_index_;
  }
  return true;
}

// Binary search in restart array to find the first restart point that
// is either the last restart point with a key less than target,
// which means the key of next restart point is larger than target, or
// the first restart point with a key = target
bool BlockIter::BinarySeek(const Slice& target, uint32_t left, uint32_t right,
                           uint32_t* index, const Comparator* comp) {
  assert(left <= right);

  while (left < right) {
    uint32_t mid = (left + right + 1) / 2;
    uint32_t region_offset = GetRestartPoint(mid);
    uint32_t shared, non_shared, value_length;
    const char* key_ptr = DecodeEntry(data_ + region_offset, data_ + restarts_,
                                      &shared, &non_shared, &value_length);
    if (key_ptr == nullptr || (shared != 0)) {
      CorruptionError();
      return false;
    }
    Slice mid_key(key_ptr, non_shared);
    int cmp = comp->Compare(mid_key, target);
    if (cmp < 0) {
      // Key at "mid" is smaller than "target". Therefore all
      // blocks before "mid" are uninteresting.
      left = mid;
    } else if (cmp > 0) {
      // Key at "mid" is >= "target". Therefore all blocks at or
      // after "mid" are uninteresting.
      right = mid - 1;
    } else {
      left = right = mid;
    }
  }

  *index = left;
  return true;
}

// Compare target key and the block key of the block of `block_index`.
// Return -1 if error.
int IndexBlockIter::CompareBlockKey(uint32_t block_index, const Slice& target) {
  uint32_t region_offset = GetRestartPoint(block_index);
  uint32_t shared, non_shared, value_length;
  const char* key_ptr = DecodeEntry(data_ + region_offset, data_ + restarts_,
                                    &shared, &non_shared, &value_length);
  if (key_ptr == nullptr || (shared != 0)) {
    CorruptionError();
    return 1;  // Return target is smaller
  }
  Slice block_key(key_ptr, non_shared);
  return Compare(block_key, target);
}

// Binary search in block_ids to find the first block
// with a key >= target
bool IndexBlockIter::BinaryBlockIndexSeek(const Slice& target,
                                          uint32_t* block_ids, uint32_t left,
                                          uint32_t right, uint32_t* index) {
  assert(left <= right);
  uint32_t left_bound = left;

  while (left <= right) {
    uint32_t mid = (right + left) / 2;

    int cmp = CompareBlockKey(block_ids[mid], target);
    if (!status_.ok()) {
      return false;
    }
    if (cmp < 0) {
      // Key at "target" is larger than "mid". Therefore all
      // blocks before or at "mid" are uninteresting.
      left = mid + 1;
    } else {
      // Key at "target" is <= "mid". Therefore all blocks
      // after "mid" are uninteresting.
      // If there is only one block left, we found it.
      if (left == right) break;
      right = mid;
    }
  }

  if (left == right) {
    // In one of the two following cases:
    // (1) left is the first one of block_ids
    // (2) there is a gap of blocks between block of `left` and `left-1`.
    // we can further distinguish the case of key in the block or key not
    // existing, by comparing the target key and the key of the previous
    // block to the left of the block found.
    if (block_ids[left] > 0 &&
        (left == left_bound || block_ids[left - 1] != block_ids[left] - 1) &&
        CompareBlockKey(block_ids[left] - 1, target) > 0) {
      current_ = restarts_;
      return false;
    }

    *index = block_ids[left];
    return true;
  } else {
    assert(left > right);
    // Mark iterator invalid
    current_ = restarts_;
    return false;
  }
}

bool IndexBlockIter::PrefixSeek(const Slice& target, uint32_t* index) {
  assert(prefix_index_);
  Slice seek_key = target;
  if (!key_includes_seq_) {
    seek_key = ExtractUserKey(target);
  }
  uint32_t* block_ids = nullptr;
  uint32_t num_blocks = prefix_index_->GetBlocks(target, &block_ids);

  if (num_blocks == 0) {
    current_ = restarts_;
    return false;
  } else  {
    return BinaryBlockIndexSeek(seek_key, block_ids, 0, num_blocks - 1, index);
  }
}

// NOTE: in hash seek, if the key is not found in the restart intervals,
// the iterator will simply be set as "invalid", rather than returning
// the key that is just pass the target key.
// return value: found
bool DataBlockIter::HashSeek(const Slice& target) {
  assert(data_block_hash_index_);
  Slice user_key = ExtractUserKey(target);
  DataBlockHashIndexIterator data_block_hash_iter;
  data_block_hash_index_->NewIterator(&data_block_hash_iter, user_key);

  for (; data_block_hash_iter.Valid(); data_block_hash_iter.Next()) {
    uint32_t restart_index = data_block_hash_iter.Value();
    SeekToRestartPoint(restart_index);
    while (true) {
      // Here we only linear seek the target key inside the restart interval.
      // When we check each [TAG restart_index] pair in the DataBlockHashIndex
      // bucket, if a key does not exist inside a restart interval, we avoid
      // further searching the block content accross restart interval boundary.
      // Rather, we check the next restart_index in the hash bucket that has
      // a matching TAG.
      //
      // TODO(fwu): check the left and write boundary of the restart interval
      // to avoid linear seek a target key that is out of range.
      if (!ParseNextDataKey(true /*within_restart_interval*/) ||
          Compare(key_, target) >= 0) {
        break;
      }
    }
    if ((current_ != restarts_) /* valid */ &&
        // If the user key portion match do we consider key_ matches
        // Currently we ignore the seq_num, so not supporting snapshot Get().
        // TODO(fwu) support snapshot Get().
        user_comparator_->Compare(key_.GetUserKey(), user_key) == 0) {
      return true;  // found
    }
  }

  current_ = restarts_;  // not found, Invalidate the iterator
  return false;
}

uint32_t Block::NumRestarts() const {
  assert(size_ >= 2*sizeof(uint32_t));
  uint32_t block_footer = DecodeFixed32(data_ + size_ - sizeof(uint32_t));
  return block_footer & 0x7FFFFFFF;
}

uint32_t Block::IndexType() const {
  assert(size_ >= 2 * sizeof(uint32_t));
  uint32_t block_footer = DecodeFixed32(data_ + size_ - sizeof(uint32_t));
  return block_footer >> 31;
}

Block::Block(BlockContents&& contents, SequenceNumber _global_seqno,
             size_t read_amp_bytes_per_bit, Statistics* statistics)
    : contents_(std::move(contents)),
      data_(contents_.data.data()),
      size_(contents_.data.size()),
      restart_offset_(0),
      num_restarts_(0),
      global_seqno_(_global_seqno) {
  if (size_ < sizeof(uint32_t)) {
    size_ = 0;  // Error marker
  } else {
    // Should only decode restart points for uncompressed blocks
    if (compression_type() == kNoCompression) {
      num_restarts_ = NumRestarts();
      switch (IndexType()) {
        case BlockBasedTableOptions::kDataBlockBinarySearch:
          restart_offset_ = static_cast<uint32_t>(size_) -
                            (1 + num_restarts_) * sizeof(uint32_t);
          if (restart_offset_ > size_ - sizeof(uint32_t)) {
            // The size is too small for NumRestarts() and therefore
            // restart_offset_ wrapped around.
            size_ = 0;
          }
          break;
        case BlockBasedTableOptions::kDataBlockHashSearch:
          if (size_ < sizeof(uint32_t) /* NUM_RESTARTS*/ +
                          sizeof(uint16_t) * 2 /* NUM_BUCK and MAP_START */) {
            size_ = 0;
            break;
          }

          data_block_hash_index_.Initialize(
              Slice(contents.data.data(), /* chop off NUM_RESTARTS */
                    contents.data.size() - sizeof(uint32_t)));

          restart_offset_ = data_block_hash_index_.DataBlockHashMapStart() -
                            num_restarts_ * sizeof(uint32_t);

          if (restart_offset_ >
              data_block_hash_index_.DataBlockHashMapStart()) {
            // DataBlockHashMapStart() is too small for NumRestarts() and
            // therefore restart_offset_ wrapped around.
            size_ = 0;
            break;
          }
          break;
        default:
          size_ = 0;  // Error marker
      }
    }
  }
  if (read_amp_bytes_per_bit != 0 && statistics && size_ != 0) {
    read_amp_bitmap_.reset(new BlockReadAmpBitmap(
        restart_offset_, read_amp_bytes_per_bit, statistics));
  }
}

template <>
DataBlockIter* Block::NewIterator(const Comparator* cmp, const Comparator* ucmp,
                                  DataBlockIter* iter, Statistics* stats,
                                  bool /*total_order_seek*/,
                                  bool /*key_includes_seq*/,
                                  BlockPrefixIndex* /*prefix_index*/,
                                  bool is_data_block_point_lookup) {
  DataBlockIter* ret_iter;
  if (iter != nullptr) {
    ret_iter = iter;
  } else {
    ret_iter = new DataBlockIter;
  }
  if (size_ < 2 * sizeof(uint32_t)) {
    ret_iter->Invalidate(Status::Corruption("bad block contents"));
    return ret_iter;
  }
  if (num_restarts_ == 0) {
    // Empty block.
    ret_iter->Invalidate(Status::OK());
    return ret_iter;
  } else {
    // We can use HashIndex only if both conditions are satisfied:
    // 1) the caller is doing point lookup
    //      i.e. is_data_block_point_lookup == true;
    // 2) the embedded HashIndex instance should have been initialized
    //      i.e. InUse() = true.
    bool using_hash_index = is_data_block_point_lookup &&
      data_block_hash_index_.InUse();
    ret_iter->Initialize(cmp, ucmp, data_, restart_offset_, num_restarts_,
                         global_seqno_, read_amp_bitmap_.get(), cachable(),
                         using_hash_index ? &data_block_hash_index_ : nullptr);
    if (read_amp_bitmap_) {
      if (read_amp_bitmap_->GetStatistics() != stats) {
        // DB changed the Statistics pointer, we need to notify read_amp_bitmap_
        read_amp_bitmap_->SetStatistics(stats);
      }
    }
  }

  return ret_iter;
}

template <>
IndexBlockIter* Block::NewIterator(const Comparator* cmp,
                                   const Comparator* ucmp, IndexBlockIter* iter,
                                   Statistics* /*stats*/, bool total_order_seek,
                                   bool key_includes_seq,
                                   BlockPrefixIndex* prefix_index,
                                   bool /* is_data_block_point_lookup */) {
  IndexBlockIter* ret_iter;
  if (iter != nullptr) {
    ret_iter = iter;
  } else {
    ret_iter = new IndexBlockIter;
  }
  if (size_ < 2 * sizeof(uint32_t)) {
    ret_iter->Invalidate(Status::Corruption("bad block contents"));
    return ret_iter;
  }
  if (num_restarts_ == 0) {
    // Empty block.
    ret_iter->Invalidate(Status::OK());
    return ret_iter;
  } else {
    BlockPrefixIndex* prefix_index_ptr =
        total_order_seek ? nullptr : prefix_index;
    ret_iter->Initialize(cmp, ucmp, data_, restart_offset_, num_restarts_,
                         prefix_index_ptr, key_includes_seq, cachable(),
                         nullptr /* data_block_hash_index */);
  }

  return ret_iter;
}

size_t Block::ApproximateMemoryUsage() const {
  size_t usage = usable_size();
#ifdef ROCKSDB_MALLOC_USABLE_SIZE
  usage += malloc_usable_size((void*)this);
#else
  usage += sizeof(*this);
#endif  // ROCKSDB_MALLOC_USABLE_SIZE
  if (read_amp_bitmap_) {
    usage += read_amp_bitmap_->ApproximateMemoryUsage();
  }
  return usage;
}

}  // namespace rocksdb
