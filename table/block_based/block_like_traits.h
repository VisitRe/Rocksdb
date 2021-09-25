//  Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include "cache/cache_entry_roles.h"
#include "port/lang.h"
#include "table/block_based/block.h"
#include "table/block_based/block_type.h"
#include "table/block_based/parsed_full_filter_block.h"
#include "table/format.h"

namespace ROCKSDB_NAMESPACE {
// concept BlockLikeOptions {
//  virtual Statistics* GetStatistics() const = 0;
//  virtual bool UsingZstd() const = 0;
//  virtual const FilterPolicy* GetFilterPolicy() const = 0;
//  virtual size_t GetReadAmpBytesPerBit() const = 0;
//  virtual bool IsIndexDeltaEncoded() const = 0;
//};

template <typename TBlocklike>
class BlocklikeTraits;

template <typename T, CacheEntryRole R>
Cache::CacheItemHelper* GetCacheItemHelperForRole();

template <typename TBlocklike, typename BlockLikeOptions>
Cache::CreateCallback GetCreateCallback(const BlockLikeOptions* options) {
  return [options](void* buf, size_t size, void** out_obj,
                   size_t* charge) -> Status {
    assert(buf != nullptr);
    std::unique_ptr<char[]> buf_data(new char[size]());
    memcpy(buf_data.get(), buf, size);
    BlockContents bc = BlockContents(std::move(buf_data), size);
    TBlocklike* ucd_ptr =
        BlocklikeTraits<TBlocklike>::Create(std::move(bc), options);
    *out_obj = reinterpret_cast<void*>(ucd_ptr);
    *charge = size;
    return Status::OK();
  };
}

template <>
class BlocklikeTraits<BlockContents> {
 public:
  template <typename BlockLikeOptions>
  static BlockContents* Create(BlockContents&& contents,
                               const BlockLikeOptions* /* options*/) {
    return new BlockContents(std::move(contents));
  }

  static uint32_t GetNumRestarts(const BlockContents& /* contents */) {
    return 0;
  }

  static size_t SizeCallback(void* obj) {
    assert(obj != nullptr);
    BlockContents* ptr = static_cast<BlockContents*>(obj);
    return ptr->data.size();
  }

  static Status SaveToCallback(void* from_obj, size_t from_offset,
                               size_t length, void* out) {
    assert(from_obj != nullptr);
    BlockContents* ptr = static_cast<BlockContents*>(from_obj);
    const char* buf = ptr->data.data();
    assert(length == ptr->data.size());
    (void)from_offset;
    memcpy(out, buf, length);
    return Status::OK();
  }

  static Cache::CacheItemHelper* GetCacheItemHelper(BlockType block_type) {
    if (block_type == BlockType::kFilter) {
      return GetCacheItemHelperForRole<
          BlockContents, CacheEntryRole::kDeprecatedFilterBlock>();
    } else {
      // E.g. compressed cache
      return GetCacheItemHelperForRole<BlockContents,
                                       CacheEntryRole::kOtherBlock>();
    }
  }
};

template <>
class BlocklikeTraits<ParsedFullFilterBlock> {
 public:
  template <typename BlockLikeOptions>
  static ParsedFullFilterBlock* Create(BlockContents&& contents,
                                       const BlockLikeOptions* options) {
    return new ParsedFullFilterBlock(options->GetFilterPolicy(),
                                     std::move(contents));
  }

  static uint32_t GetNumRestarts(const ParsedFullFilterBlock& /* block */) {
    return 0;
  }

  static size_t SizeCallback(void* obj) {
    assert(obj != nullptr);
    ParsedFullFilterBlock* ptr = static_cast<ParsedFullFilterBlock*>(obj);
    return ptr->GetBlockContentsData().size();
  }

  static Status SaveToCallback(void* from_obj, size_t from_offset,
                               size_t length, void* out) {
    assert(from_obj != nullptr);
    ParsedFullFilterBlock* ptr = static_cast<ParsedFullFilterBlock*>(from_obj);
    const char* buf = ptr->GetBlockContentsData().data();
    assert(length == ptr->GetBlockContentsData().size());
    (void)from_offset;
    memcpy(out, buf, length);
    return Status::OK();
  }

  static Cache::CacheItemHelper* GetCacheItemHelper(BlockType block_type) {
    (void)block_type;
    assert(block_type == BlockType::kFilter);
    return GetCacheItemHelperForRole<ParsedFullFilterBlock,
                                     CacheEntryRole::kFilterBlock>();
  }
};

template <>
class BlocklikeTraits<DataBlock> {
 public:
  template <typename BlockLikeOptions>
  static DataBlock* Create(BlockContents&& contents,
                           const BlockLikeOptions* options) {
    return new DataBlock(std::move(contents), options->GetReadAmpBytesPerBit(),
                         options->GetStatistics());
  }

  static uint32_t GetNumRestarts(const DataBlock& block) {
    return block.NumRestarts();
  }

  static size_t SizeCallback(void* obj) {
    assert(obj != nullptr);
    DataBlock* ptr = static_cast<DataBlock*>(obj);
    return ptr->block_size();
  }

  static Status SaveToCallback(void* from_obj, size_t from_offset,
                               size_t length, void* out) {
    assert(from_obj != nullptr);
    Block* ptr = static_cast<Block*>(from_obj);
    const char* buf = ptr->block_data();
    assert(length == ptr->block_size());
    (void)from_offset;
    memcpy(out, buf, length);
    return Status::OK();
  }

  static Cache::CacheItemHelper* GetCacheItemHelper(BlockType block_type) {
    switch (block_type) {
      case BlockType::kData:
        return GetCacheItemHelperForRole<DataBlock,
                                         CacheEntryRole::kDataBlock>();
      case BlockType::kFilter:
        return GetCacheItemHelperForRole<DataBlock,
                                         CacheEntryRole::kFilterMetaBlock>();
      default:
        // Not a recognized combination
        assert(false);
        FALLTHROUGH_INTENDED;
      case BlockType::kRangeDeletion:
        return GetCacheItemHelperForRole<DataBlock,
                                         CacheEntryRole::kOtherBlock>();
    }
  }
};

template <>
class BlocklikeTraits<IndexBlock> {
 public:
  template <typename BlockLikeOptions>
  static IndexBlock* Create(BlockContents&& contents,
                            const BlockLikeOptions* options) {
    return new IndexBlock(std::move(contents), options->IsIndexDeltaEncoded());
  }

  static uint32_t GetNumRestarts(const IndexBlock& block) {
    return block.NumRestarts();
  }

  static size_t SizeCallback(void* obj) {
    assert(obj != nullptr);
    IndexBlock* ptr = static_cast<IndexBlock*>(obj);
    return ptr->block_size();
  }

  static Status SaveToCallback(void* from_obj, size_t from_offset,
                               size_t length, void* out) {
    assert(from_obj != nullptr);
    IndexBlock* ptr = static_cast<IndexBlock*>(from_obj);
    const char* buf = ptr->block_data();
    assert(length == ptr->block_size());
    (void)from_offset;
    memcpy(out, buf, length);
    return Status::OK();
  }

  static Cache::CacheItemHelper* GetCacheItemHelper(BlockType block_type) {
    switch (block_type) {
      case BlockType::kIndex:
        return GetCacheItemHelperForRole<IndexBlock,
                                         CacheEntryRole::kIndexBlock>();
      case BlockType::kFilter:
        return GetCacheItemHelperForRole<IndexBlock,
                                         CacheEntryRole::kFilterMetaBlock>();
      default:
        // Not a recognized combination
        assert(false);
        FALLTHROUGH_INTENDED;
      case BlockType::kRangeDeletion:
        return GetCacheItemHelperForRole<IndexBlock,
                                         CacheEntryRole::kOtherBlock>();
    }
  }
};

template <>
class BlocklikeTraits<MetaBlock> {
 public:
  template <typename BlockLikeOptions>
  static MetaBlock* Create(BlockContents&& contents,
                           const BlockLikeOptions* /*options*/) {
    auto block = new MetaBlock(std::move(contents));
    return block;
  }

  static uint32_t GetNumRestarts(const MetaBlock& block) {
    return block.NumRestarts();
  }

  static size_t SizeCallback(void* obj) {
    assert(obj != nullptr);
    MetaBlock* ptr = static_cast<MetaBlock*>(obj);
    return ptr->block_size();
  }

  static Status SaveToCallback(void* from_obj, size_t from_offset,
                               size_t length, void* out) {
    assert(from_obj != nullptr);
    MetaBlock* ptr = static_cast<MetaBlock*>(from_obj);
    const char* buf = ptr->block_data();
    assert(length == ptr->block_size());
    (void)from_offset;
    memcpy(out, buf, length);
    return Status::OK();
  }

  static Cache::CacheItemHelper* GetCacheItemHelper(BlockType block_type) {
    switch (block_type) {
      case BlockType::kData:
        return GetCacheItemHelperForRole<MetaBlock,
                                         CacheEntryRole::kDataBlock>();
      case BlockType::kFilter:
        return GetCacheItemHelperForRole<MetaBlock,
                                         CacheEntryRole::kFilterMetaBlock>();
      default:
        // Not a recognized combination
        assert(false);
        FALLTHROUGH_INTENDED;
      case BlockType::kRangeDeletion:
        return GetCacheItemHelperForRole<MetaBlock,
                                         CacheEntryRole::kOtherBlock>();
    }
  }
};

template <>
class BlocklikeTraits<UncompressionDict> {
 public:
  template <typename BlockLikeOptions>
  static UncompressionDict* Create(BlockContents&& contents,
                                   const BlockLikeOptions* options) {
    return new UncompressionDict(contents.data, std::move(contents.allocation),
                                 options->UsingZstd());
  }

  static uint32_t GetNumRestarts(const UncompressionDict& /* dict */) {
    return 0;
  }

  static size_t SizeCallback(void* obj) {
    assert(obj != nullptr);
    UncompressionDict* ptr = static_cast<UncompressionDict*>(obj);
    return ptr->slice_.size();
  }

  static Status SaveToCallback(void* from_obj, size_t from_offset,
                               size_t length, void* out) {
    assert(from_obj != nullptr);
    UncompressionDict* ptr = static_cast<UncompressionDict*>(from_obj);
    const char* buf = ptr->slice_.data();
    assert(length == ptr->slice_.size());
    (void)from_offset;
    memcpy(out, buf, length);
    return Status::OK();
  }

  static Cache::CacheItemHelper* GetCacheItemHelper(BlockType block_type) {
    (void)block_type;
    assert(block_type == BlockType::kCompressionDictionary);
    return GetCacheItemHelperForRole<UncompressionDict,
                                     CacheEntryRole::kOtherBlock>();
  }
};

// Get an CacheItemHelper pointer for value type T and role R.
template <typename T, CacheEntryRole R>
Cache::CacheItemHelper* GetCacheItemHelperForRole() {
  static Cache::CacheItemHelper cache_helper(
      BlocklikeTraits<T>::SizeCallback, BlocklikeTraits<T>::SaveToCallback,
      GetCacheEntryDeleterForRole<T, R>());
  return &cache_helper;
}

}  // namespace ROCKSDB_NAMESPACE
