//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "cache/compressed_secondary_cache.h"

#include <memory>

#include "memory/memory_allocator.h"
#include "util/compression.h"
#include "util/string_util.h"

namespace ROCKSDB_NAMESPACE {

namespace {

void DeletionCallback(const Slice& /*key*/, void* obj) {
  delete reinterpret_cast<CacheAllocationPtr*>(obj);
  obj = nullptr;
}

}  // namespace

CompressedSecondaryCache::CompressedSecondaryCache(
    size_t capacity, int num_shard_bits, bool strict_capacity_limit,
    double high_pri_pool_ratio,
    std::shared_ptr<MemoryAllocator> memory_allocator, bool use_adaptive_mutex,
    CacheMetadataChargePolicy metadata_charge_policy,
    CompressionType compression_type, uint32_t compress_format_version)
    : cache_options_(capacity, num_shard_bits, strict_capacity_limit,
                     high_pri_pool_ratio, memory_allocator, use_adaptive_mutex,
                     metadata_charge_policy, compression_type,
                     compress_format_version) {
  cache_ = NewLRUCache(capacity, num_shard_bits, strict_capacity_limit,
                       high_pri_pool_ratio, memory_allocator,
                       use_adaptive_mutex, metadata_charge_policy);
}

CompressedSecondaryCache::~CompressedSecondaryCache() { cache_.reset(); }

std::unique_ptr<SecondaryCacheResultHandle> CompressedSecondaryCache::Lookup(
    const Slice& key, const Cache::CreateCallback& create_cb, bool /*wait*/,
    bool& is_in_sec_cache) {
  std::unique_ptr<SecondaryCacheResultHandle> handle;
  is_in_sec_cache = false;
  Cache::Handle* lru_handle = cache_->Lookup(key);
  if (lru_handle == nullptr) {
    return handle;
  }
  size_t value_size = cache_->GetCharge(lru_handle);
  CacheAllocationPtr* ptr =
      reinterpret_cast<CacheAllocationPtr*>(cache_->Value(lru_handle));
  void* value = nullptr;
  size_t charge = 0;
  Status s;

  if (cache_options_.compression_type == kNoCompression) {
    s = create_cb(ptr->get(), cache_->GetCharge(lru_handle), &value, &charge);
  } else {
    UncompressionContext uncompression_context(cache_options_.compression_type);
    UncompressionInfo uncompression_info(uncompression_context,
                                         UncompressionDict::GetEmptyDict(),
                                         cache_options_.compression_type);

    size_t uncompressed_size = 0;
    CacheAllocationPtr uncompressed;
    uncompressed = UncompressData(
        uncompression_info, (char*)ptr->get(), cache_->GetCharge(lru_handle),
        &uncompressed_size, cache_options_.compress_format_version,
        cache_options_.memory_allocator.get());

    if (!uncompressed) {
      cache_->Release(lru_handle, /* erase_if_last_ref */ true);
      return handle;
    }
    s = create_cb(uncompressed.get(), uncompressed_size, &value, &charge);
  }

  if (!s.ok()) {
    cache_->Release(lru_handle, /* erase_if_last_ref */ true);
    return handle;
  }

  cache_->Release(lru_handle, /* erase_if_last_ref */ true);
  handle.reset(new CompressedSecondaryCacheResultHandle(value, charge));

  return handle;
}

Status CompressedSecondaryCache::Insert(const Slice& key, void* value,
                                        const Cache::CacheItemHelper* helper) {
  size_t size = (*helper->size_cb)(value);
  CacheAllocationPtr ptr =
      AllocateBlock(size, cache_options_.memory_allocator.get());

  Status s = (*helper->saveto_cb)(value, 0, size, ptr.get());
  if (!s.ok()) {
    return s;
  }
  Slice val(ptr.get(), size);

  std::string compressed_val;
  if (cache_options_.compression_type != kNoCompression) {
    CompressionOptions compression_opts;
    CompressionContext compression_context(cache_options_.compression_type);
    uint64_t sample_for_compression = 0;
    CompressionInfo compression_info(
        compression_opts, compression_context, CompressionDict::GetEmptyDict(),
        cache_options_.compression_type, sample_for_compression);

    bool success =
        CompressData(val, compression_info,
                     cache_options_.compress_format_version, &compressed_val);

    if (!success) {
      return Status::Corruption("Error compressing value.");
    }

    val = Slice(compressed_val);
    size = compressed_val.size();
    // ptr = AllocateBlock(size, cache_options_.memory_allocator.get());
    // memcpy(ptr.get(), compressed_val.data(), size);
    CacheValueChunk* val_ptr = new CacheValueChunk();
    // CompressedSecondaryCache::CacheValueChunk* val_ptr =
    SplitValueIntoChunks(compressed_val, val_ptr, size);
    ptr =
        AllocateBlock(sizeof(*val_ptr), cache_options_.memory_allocator.get());
    memcpy(ptr.get(), val_ptr, sizeof(*val_ptr));
  }

  CacheAllocationPtr* buf = new CacheAllocationPtr(std::move(ptr));

  return cache_->Insert(key, buf, size, DeletionCallback);
}

void CompressedSecondaryCache::Erase(const Slice& key) { cache_->Erase(key); }

std::string CompressedSecondaryCache::GetPrintableOptions() const {
  std::string ret;
  ret.reserve(20000);
  const int kBufferSize = 200;
  char buffer[kBufferSize];
  ret.append(cache_->GetPrintableOptions());
  snprintf(buffer, kBufferSize, "    compression_type : %s\n",
           CompressionTypeToString(cache_options_.compression_type).c_str());
  ret.append(buffer);
  snprintf(buffer, kBufferSize, "    compression_type : %d\n",
           cache_options_.compress_format_version);
  ret.append(buffer);
  return ret;
}

void CompressedSecondaryCache::SplitValueIntoChunks(const std::string& value,
                                                    CacheValueChunk* chunk_ptr,
                                                    size_t& size) {
  assert(!value.empty());
  // TODO how to deal with empty value
  auto src_ptr = value.data();
  // for proper size, memcpy chunks into CompressedSecondaryCacheValue
  size_t src_size = size;
  CacheAllocationPtr ptr;

  CacheValueChunk* current_chunk = chunk_ptr;
  while (src_size > 0) {
    size += sizeof(current_chunk);
    if (src_size <= malloc_bin_sizes_.front()) {
      ptr = AllocateBlock(src_size, cache_options_.memory_allocator.get());
      memcpy(ptr.get(), src_ptr, src_size);
      current_chunk->chunk_ptr = new CacheAllocationPtr(std::move(ptr));
      current_chunk->size = src_size;
      src_size = 0;
    } else if (src_size >= malloc_bin_sizes_.back()) {
      current_chunk->size = malloc_bin_sizes_.back();
      ptr = AllocateBlock(current_chunk->size,
                          cache_options_.memory_allocator.get());
      memcpy(ptr.get(), src_ptr, current_chunk->size);
      current_chunk->chunk_ptr = new CacheAllocationPtr(std::move(ptr));
      src_size -= current_chunk->size;
      src_ptr += current_chunk->size;
    } else {
      for (size_t i = 0; i < malloc_bin_sizes_.size() - 1; ++i) {
        if (src_size >= malloc_bin_sizes_[i] &&
            src_size >= malloc_bin_sizes_[i + 1]) {
          current_chunk->size = malloc_bin_sizes_[i];
          ptr = AllocateBlock(current_chunk->size,
                              cache_options_.memory_allocator.get());
          memcpy(ptr.get(), src_ptr, current_chunk->size);
          current_chunk->chunk_ptr = new CacheAllocationPtr(std::move(ptr));
          src_size -= current_chunk->size;
          src_ptr += current_chunk->size;
        }
      }
    }
    if (src_size > 0) {
      CacheValueChunk* next_chunk = new CacheValueChunk();
      current_chunk->next = next_chunk;
      current_chunk = next_chunk;
    }
  }
}

std::string* CompressedSecondaryCache::MergeChunks(void* chunk, size_t size) {}

std::shared_ptr<SecondaryCache> NewCompressedSecondaryCache(
    size_t capacity, int num_shard_bits, bool strict_capacity_limit,
    double high_pri_pool_ratio,
    std::shared_ptr<MemoryAllocator> memory_allocator, bool use_adaptive_mutex,
    CacheMetadataChargePolicy metadata_charge_policy,
    CompressionType compression_type, uint32_t compress_format_version) {
  return std::make_shared<CompressedSecondaryCache>(
      capacity, num_shard_bits, strict_capacity_limit, high_pri_pool_ratio,
      memory_allocator, use_adaptive_mutex, metadata_charge_policy,
      compression_type, compress_format_version);
}

std::shared_ptr<SecondaryCache> NewCompressedSecondaryCache(
    const CompressedSecondaryCacheOptions& opts) {
  // The secondary_cache is disabled for this LRUCache instance.
  assert(opts.secondary_cache == nullptr);
  return NewCompressedSecondaryCache(
      opts.capacity, opts.num_shard_bits, opts.strict_capacity_limit,
      opts.high_pri_pool_ratio, opts.memory_allocator, opts.use_adaptive_mutex,
      opts.metadata_charge_policy, opts.compression_type,
      opts.compress_format_version);
}

}  // namespace ROCKSDB_NAMESPACE
