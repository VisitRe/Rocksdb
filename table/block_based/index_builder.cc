//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/block_based/index_builder.h"

#include <assert.h>

#include <cinttypes>
#include <list>
#include <string>

#include "db/dbformat.h"
#include "rocksdb/comparator.h"
#include "rocksdb/flush_block_policy.h"
#include "table/block_based/partitioned_filter_block.h"
#include "table/format.h"

namespace ROCKSDB_NAMESPACE {

// Create a index builder based on its type.
IndexBuilder* IndexBuilder::CreateIndexBuilder(
    BlockBasedTableOptions::IndexType index_type,
    const InternalKeyComparator* comparator,
    const InternalKeySliceTransform* int_key_slice_transform,
    const bool use_value_delta_encoding,
    const BlockBasedTableOptions& table_opt) {
  IndexBuilder* result = nullptr;
  switch (index_type) {
    case BlockBasedTableOptions::kBinarySearch: {
      result = new ShortenedIndexBuilder(
          comparator, table_opt.index_block_restart_interval,
          table_opt.format_version, use_value_delta_encoding,
          table_opt.index_shortening, /* include_first_key */ false,
          table_opt.max_index_size);
      break;
    }
    case BlockBasedTableOptions::kHashSearch: {
      // Currently kHashSearch is incompatible with index_block_restart_interval
      // > 1
      assert(table_opt.index_block_restart_interval == 1);
      result = new HashIndexBuilder(
          comparator, int_key_slice_transform,
          table_opt.index_block_restart_interval, table_opt.format_version,
          use_value_delta_encoding, table_opt.index_shortening);
      break;
    }
    case BlockBasedTableOptions::kTwoLevelIndexSearch: {
      result = PartitionedIndexBuilder::CreateIndexBuilder(
          comparator, use_value_delta_encoding, table_opt);
      break;
    }
    case BlockBasedTableOptions::kBinarySearchWithFirstKey: {
      result = new ShortenedIndexBuilder(
          comparator, table_opt.index_block_restart_interval,
          table_opt.format_version, use_value_delta_encoding,
          table_opt.index_shortening, /* include_first_key */ true,
          table_opt.max_index_size);
      break;
    }
    default: {
      assert(!"Do not recognize the index type ");
      break;
    }
  }
  return result;
}

void ShortenedIndexBuilder::FindShortestInternalKeySeparator(
    const Comparator& comparator, std::string* start, const Slice& limit) {
  // Attempt to shorten the user portion of the key
  Slice user_start = ExtractUserKey(*start);
  Slice user_limit = ExtractUserKey(limit);
  std::string tmp(user_start.data(), user_start.size());
  comparator.FindShortestSeparator(&tmp, user_limit);
  if (tmp.size() <= user_start.size() &&
      comparator.Compare(user_start, tmp) < 0) {
    // User key has become shorter physically, but larger logically.
    // Tack on the earliest possible number to the shortened user key.
    PutFixed64(&tmp,
               PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
    assert(InternalKeyComparator(&comparator).Compare(*start, tmp) < 0);
    assert(InternalKeyComparator(&comparator).Compare(tmp, limit) < 0);
    start->swap(tmp);
  }
}

void ShortenedIndexBuilder::FindShortInternalKeySuccessor(
    const Comparator& comparator, std::string* key) {
  Slice user_key = ExtractUserKey(*key);
  std::string tmp(user_key.data(), user_key.size());
  comparator.FindShortSuccessor(&tmp);
  if (tmp.size() <= user_key.size() && comparator.Compare(user_key, tmp) < 0) {
    // User key has become shorter physically, but larger logically.
    // Tack on the earliest possible number to the shortened user key.
    PutFixed64(&tmp,
               PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
    assert(InternalKeyComparator(&comparator).Compare(*key, tmp) < 0);
    key->swap(tmp);
  }
}

PartitionedIndexBuilder* PartitionedIndexBuilder::CreateIndexBuilder(
    const InternalKeyComparator* comparator,
    const bool use_value_delta_encoding,
    const BlockBasedTableOptions& table_opt) {
  return new PartitionedIndexBuilder(comparator, table_opt,
                                     use_value_delta_encoding);
}

PartitionedIndexBuilder::PartitionedIndexBuilder(
    const InternalKeyComparator* comparator,
    const BlockBasedTableOptions& table_opt,
    const bool use_value_delta_encoding)
    : IndexBuilder(comparator),
      index_block_builder_(table_opt.index_block_restart_interval,
                           true /*use_delta_encoding*/,
                           use_value_delta_encoding),
      index_block_builder_without_seq_(table_opt.index_block_restart_interval,
                                       true /*use_delta_encoding*/,
                                       use_value_delta_encoding),
      sub_index_builder_(nullptr),
      table_opt_(table_opt),
      // We start by false. After each partition we revise the value based on
      // what the sub_index_builder has decided. If the feature is disabled
      // entirely, this will be set to true after switching the first
      // sub_index_builder. Otherwise, it could be set to true even one of the
      // sub_index_builders could not safely exclude seq from the keys, then it
      // wil be enforced on all sub_index_builders on ::Finish.
      seperator_is_key_plus_seq_(false),
      use_value_delta_encoding_(use_value_delta_encoding),
      current_index_size_(0),
      current_top_level_index_raw_key_size_(0) {}

PartitionedIndexBuilder::~PartitionedIndexBuilder() {
  delete sub_index_builder_;
}

void PartitionedIndexBuilder::MakeNewSubIndexBuilder() {
  assert(sub_index_builder_ == nullptr);
  sub_index_builder_ = new ShortenedIndexBuilder(
      comparator_, table_opt_.index_block_restart_interval,
      table_opt_.format_version, use_value_delta_encoding_,
      table_opt_.index_shortening, /* include_first_key */ false, ULLONG_MAX);

  // Set sub_index_builder_->seperator_is_key_plus_seq_ to true if
  // seperator_is_key_plus_seq_ is true (internal-key mode) (set to false by
  // default on Creation) so that flush policy can point to
  // sub_index_builder_->index_block_builder_
  if (seperator_is_key_plus_seq_) {
    sub_index_builder_->seperator_is_key_plus_seq_ = true;
  }

  flush_policy_.reset(FlushBlockBySizePolicyFactory::NewFlushBlockPolicy(
      table_opt_.metadata_block_size, table_opt_.block_size_deviation,
      // Note: this is sub-optimal since sub_index_builder_ could later reset
      // seperator_is_key_plus_seq_ but the probability of that is low.
      sub_index_builder_->seperator_is_key_plus_seq_
          ? sub_index_builder_->index_block_builder_
          : sub_index_builder_->index_block_builder_without_seq_));
  partition_cut_requested_ = false;
}

void PartitionedIndexBuilder::RequestPartitionCut() {
  partition_cut_requested_ = true;
}

void PartitionedIndexBuilder::AddIndexEntry(
    std::string* last_key_in_current_block,
    const Slice* first_key_in_next_block, const BlockHandle& block_handle) {
  // Note: to avoid two consecuitive flush in the same method call, we do not
  // check flush policy when adding the last key
  if (UNLIKELY(first_key_in_next_block == nullptr)) {  // no more keys
    if (sub_index_builder_ == nullptr) {
      MakeNewSubIndexBuilder();
    }
    sub_index_builder_->AddIndexEntry(last_key_in_current_block,
                                      first_key_in_next_block, block_handle);
    if (!seperator_is_key_plus_seq_ &&
        sub_index_builder_->seperator_is_key_plus_seq_) {
      // then we need to apply it to all sub-index builders and reset
      // flush_policy to point to Block Builder of sub_index_builder_ that store
      // internal keys.
      seperator_is_key_plus_seq_ = true;
      flush_policy_.reset(FlushBlockBySizePolicyFactory::NewFlushBlockPolicy(
          table_opt_.metadata_block_size, table_opt_.block_size_deviation,
          sub_index_builder_->index_block_builder_));
    }
    sub_index_last_key_ = std::string(*last_key_in_current_block);
    entries_.push_back(
        {sub_index_last_key_,
         std::unique_ptr<ShortenedIndexBuilder>(sub_index_builder_)});
    current_top_level_index_raw_key_size_ += sub_index_last_key_.size();
    current_index_size_ +=
        sub_index_builder_->index_block_builder_.CurrentSizeEstimate();
    sub_index_builder_ = nullptr;
    cut_filter_block = true;
  } else {
    // apply flush policy only to non-empty sub_index_builder_
    if (sub_index_builder_ != nullptr) {
      std::string handle_encoding;
      block_handle.EncodeTo(&handle_encoding);
      bool do_flush =
          partition_cut_requested_ ||
          flush_policy_->Update(*last_key_in_current_block, handle_encoding);
      if (do_flush) {
        entries_.push_back(
            {sub_index_last_key_,
             std::unique_ptr<ShortenedIndexBuilder>(sub_index_builder_)});
        cut_filter_block = true;
        current_index_size_ +=
            sub_index_builder_->index_block_builder_.CurrentSizeEstimate();
        current_top_level_index_raw_key_size_ += sub_index_last_key_.size();
        sub_index_builder_ = nullptr;
      }
    }
    if (sub_index_builder_ == nullptr) {
      MakeNewSubIndexBuilder();
    }
    sub_index_builder_->AddIndexEntry(last_key_in_current_block,
                                      first_key_in_next_block, block_handle);
    sub_index_last_key_ = std::string(*last_key_in_current_block);
    if (!seperator_is_key_plus_seq_ &&
        sub_index_builder_->seperator_is_key_plus_seq_) {
      // then we need to apply it to all sub-index builders and reset
      // flush_policy to point to Block Builder of sub_index_builder_ that store
      // internal keys.
      seperator_is_key_plus_seq_ = true;
      flush_policy_.reset(FlushBlockBySizePolicyFactory::NewFlushBlockPolicy(
          table_opt_.metadata_block_size, table_opt_.block_size_deviation,
          sub_index_builder_->index_block_builder_));
    }
  }
}

Status PartitionedIndexBuilder::Finish(
    IndexBlocks* index_blocks, const BlockHandle& last_partition_block_handle) {
  if (partition_cnt_ == 0) {
    partition_cnt_ = entries_.size();
  }
  // It must be set to null after last key is added
  assert(sub_index_builder_ == nullptr);
  if (finishing_indexes == true) {
    Entry& last_entry = entries_.front();
    std::string handle_encoding;
    last_partition_block_handle.EncodeTo(&handle_encoding);
    std::string handle_delta_encoding;
    PutVarsignedint64(
        &handle_delta_encoding,
        last_partition_block_handle.size() - last_encoded_handle_.size());
    last_encoded_handle_ = last_partition_block_handle;
    const Slice handle_delta_encoding_slice(handle_delta_encoding);
    index_block_builder_.Add(last_entry.key, handle_encoding,
                             &handle_delta_encoding_slice);
    if (!seperator_is_key_plus_seq_) {
      index_block_builder_without_seq_.Add(ExtractUserKey(last_entry.key),
                                           handle_encoding,
                                           &handle_delta_encoding_slice);
    }
    entries_.pop_front();
  }
  // If there is no sub_index left, then return the 2nd level index.
  if (UNLIKELY(entries_.empty())) {
    if (seperator_is_key_plus_seq_) {
      index_blocks->index_block_contents = index_block_builder_.Finish();
    } else {
      index_blocks->index_block_contents =
          index_block_builder_without_seq_.Finish();
    }
    top_level_index_size_ = index_blocks->index_block_contents.size();
    index_size_ += top_level_index_size_;
    return Status::OK();
  } else {
    // Finish the next partition index in line and Incomplete() to indicate we
    // expect more calls to Finish
    Entry& entry = entries_.front();
    // Apply the policy to all sub-indexes
    entry.value->seperator_is_key_plus_seq_ = seperator_is_key_plus_seq_;
    auto s = entry.value->Finish(index_blocks);
    index_size_ += index_blocks->index_block_contents.size();
    finishing_indexes = true;
    return s.ok() ? Status::Incomplete() : s;
  }
}

size_t PartitionedIndexBuilder::NumPartitions() const { return partition_cnt_; }

bool PartitionedIndexBuilder::NeedSplit() const {
  return current_index_size_ > table_opt_.max_index_size ||
         current_top_level_index_raw_key_size_ >
             table_opt_.max_top_level_index_raw_key_size;
}

}  // namespace ROCKSDB_NAMESPACE
