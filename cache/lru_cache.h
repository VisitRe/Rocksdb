//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
#pragma once

#include <memory>
#include <string>

#include "cache/sharded_cache.h"
#include "port/lang.h"
#include "port/malloc.h"
#include "port/port.h"
#include "rocksdb/secondary_cache.h"
#include "util/autovector.h"
#include "util/distributed_mutex.h"

namespace ROCKSDB_NAMESPACE {
namespace lru_cache {

// LRU cache implementation. This class is not thread-safe.

// An entry is a variable length heap-allocated structure.
// Entries are referenced by cache and/or by any external entity.
// The cache keeps all its entries in a hash table. Some elements
// are also stored on LRU list.
//
// LRUHandle can be in these states:
// 1. Referenced externally AND in hash table.
//    In that case the entry is *not* in the LRU list
//    (refs >= 1 && in_cache == true)
// 2. Not referenced externally AND in hash table.
//    In that case the entry is in the LRU list and can be freed.
//    (refs == 0 && in_cache == true)
// 3. Referenced externally AND not in hash table.
//    In that case the entry is not in the LRU list and not in hash table.
//    The entry must be freed if refs becomes 0 in this state.
//    (refs >= 1 && in_cache == false)
// If you call LRUCacheShard::Release enough times on an entry in state 1, it
// will go into state 2. To move from state 1 to state 3, either call
// LRUCacheShard::Erase or LRUCacheShard::Insert with the same key (but
// possibly different value). To move from state 2 to state 1, use
// LRUCacheShard::Lookup.
// While refs > 0, public properties like value and deleter must not change.

struct LRUHandle {
  void* value;
  union Info {
    Info() {}
    ~Info() {}
    Cache::DeleterFn deleter;
    const Cache::CacheItemHelper* helper;
  } info_;
  // An entry is not added to the LRUHandleTable until the secondary cache
  // lookup is complete, so its safe to have this union.
  union {
    LRUHandle* next_hash;
    SecondaryCacheResultHandle* sec_handle;
  };
  LRUHandle* next;
  LRUHandle* prev;
  size_t total_charge;  // TODO(opt): Only allow uint32_t?
  size_t key_length;
  // The hash of key(). Used for fast sharding and comparisons.
  uint32_t hash;
  // The number of external refs to this entry. The cache itself is not counted.
  uint32_t refs;

  // Mutable flags - access controlled by mutex
  // The m_ and M_ prefixes (and im_ and IM_ later) are to hopefully avoid
  // checking an M_ flag on im_flags or an IM_ flag on m_flags.
  uint8_t m_flags;
  enum MFlags : uint8_t {
    // Whether this entry is referenced by the hash table.
    M_IN_CACHE = (1 << 0),
    // Whether this entry has had any lookups (hits).
    M_HAS_HIT = (1 << 1),
    // Whether this entry is in high-pri pool.
    M_IN_HIGH_PRI_POOL = (1 << 2),
    // Whether this entry is in low-pri pool.
    M_IN_LOW_PRI_POOL = (1 << 3),
  };

  // "Immutable" flags - only set in single-threaded context and then
  // can be accessed without mutex
  uint8_t im_flags;
  enum ImFlags : uint8_t {
    // Whether this entry is high priority entry.
    IM_IS_HIGH_PRI = (1 << 0),
    // Whether this entry is low priority entry.
    IM_IS_LOW_PRI = (1 << 1),
    // Can this be inserted into the secondary cache.
    IM_IS_SECONDARY_CACHE_COMPATIBLE = (1 << 2),
    // Is the handle still being read from a lower tier.
    IM_IS_PENDING = (1 << 3),
    // Whether this handle is still in a lower tier
    IM_IS_IN_SECONDARY_CACHE = (1 << 4),
    // Marks result handles that should not be inserted into cache
    IM_IS_STANDALONE = (1 << 5),
  };

  // Beginning of the key (MUST BE THE LAST FIELD IN THIS STRUCT!)
  char key_data[1];

  Slice key() const { return Slice(key_data, key_length); }

  // For HandleImpl concept
  uint32_t GetHash() const { return hash; }

  // Increase the reference count by 1.
  void Ref() { refs++; }

  // Just reduce the reference count by 1. Return true if it was last reference.
  bool Unref() {
    assert(refs > 0);
    refs--;
    return refs == 0;
  }

  // Return true if there are external refs, false otherwise.
  bool HasRefs() const { return refs > 0; }

  bool InCache() const { return m_flags & M_IN_CACHE; }
  bool IsHighPri() const { return im_flags & IM_IS_HIGH_PRI; }
  bool InHighPriPool() const { return m_flags & M_IN_HIGH_PRI_POOL; }
  bool IsLowPri() const { return im_flags & IM_IS_LOW_PRI; }
  bool InLowPriPool() const { return m_flags & M_IN_LOW_PRI_POOL; }
  bool HasHit() const { return m_flags & M_HAS_HIT; }
  bool IsSecondaryCacheCompatible() const {
    return im_flags & IM_IS_SECONDARY_CACHE_COMPATIBLE;
  }
  bool IsPending() const { return im_flags & IM_IS_PENDING; }
  bool IsInSecondaryCache() const {
    return im_flags & IM_IS_IN_SECONDARY_CACHE;
  }
  bool IsStandalone() const { return im_flags & IM_IS_STANDALONE; }

  void SetInCache(bool in_cache) {
    if (in_cache) {
      m_flags |= M_IN_CACHE;
    } else {
      m_flags &= ~M_IN_CACHE;
    }
  }

  void SetPriority(Cache::Priority priority) {
    if (priority == Cache::Priority::HIGH) {
      im_flags |= IM_IS_HIGH_PRI;
      im_flags &= ~IM_IS_LOW_PRI;
    } else if (priority == Cache::Priority::LOW) {
      im_flags &= ~IM_IS_HIGH_PRI;
      im_flags |= IM_IS_LOW_PRI;
    } else {
      im_flags &= ~IM_IS_HIGH_PRI;
      im_flags &= ~IM_IS_LOW_PRI;
    }
  }

  void SetInHighPriPool(bool in_high_pri_pool) {
    if (in_high_pri_pool) {
      m_flags |= M_IN_HIGH_PRI_POOL;
    } else {
      m_flags &= ~M_IN_HIGH_PRI_POOL;
    }
  }

  void SetInLowPriPool(bool in_low_pri_pool) {
    if (in_low_pri_pool) {
      m_flags |= M_IN_LOW_PRI_POOL;
    } else {
      m_flags &= ~M_IN_LOW_PRI_POOL;
    }
  }

  void SetHit() { m_flags |= M_HAS_HIT; }

  void SetSecondaryCacheCompatible(bool compat) {
    if (compat) {
      im_flags |= IM_IS_SECONDARY_CACHE_COMPATIBLE;
    } else {
      im_flags &= ~IM_IS_SECONDARY_CACHE_COMPATIBLE;
    }
  }

  void SetIsPending(bool pending) {
    if (pending) {
      im_flags |= IM_IS_PENDING;
    } else {
      im_flags &= ~IM_IS_PENDING;
    }
  }

  void SetIsInSecondaryCache(bool is_in_secondary_cache) {
    if (is_in_secondary_cache) {
      im_flags |= IM_IS_IN_SECONDARY_CACHE;
    } else {
      im_flags &= ~IM_IS_IN_SECONDARY_CACHE;
    }
  }

  void SetIsStandalone(bool is_standalone) {
    if (is_standalone) {
      im_flags |= IM_IS_STANDALONE;
    } else {
      im_flags &= ~IM_IS_STANDALONE;
    }
  }

  void Free() {
    assert(refs == 0);
    if (!IsSecondaryCacheCompatible() && info_.deleter) {
      (*info_.deleter)(key(), value);
    } else if (IsSecondaryCacheCompatible()) {
      if (IsPending()) {
        assert(sec_handle != nullptr);
        SecondaryCacheResultHandle* tmp_sec_handle = sec_handle;
        tmp_sec_handle->Wait();
        value = tmp_sec_handle->Value();
        delete tmp_sec_handle;
      }
      if (value) {
        (*info_.helper->del_cb)(key(), value);
      }
    }
    delete[] reinterpret_cast<char*>(this);
  }

  inline size_t CalcuMetaCharge(
      CacheMetadataChargePolicy metadata_charge_policy) const {
    if (metadata_charge_policy != kFullChargeCacheMetadata) {
      return 0;
    } else {
#ifdef ROCKSDB_MALLOC_USABLE_SIZE
      return malloc_usable_size(
          const_cast<void*>(static_cast<const void*>(this)));
#else
      // This is the size that is used when a new handle is created.
      return sizeof(LRUHandle) - 1 + key_length;
#endif
    }
  }

  // Calculate the memory usage by metadata.
  inline void CalcTotalCharge(
      size_t charge, CacheMetadataChargePolicy metadata_charge_policy) {
    total_charge = charge + CalcuMetaCharge(metadata_charge_policy);
  }

  inline size_t GetCharge(
      CacheMetadataChargePolicy metadata_charge_policy) const {
    size_t meta_charge = CalcuMetaCharge(metadata_charge_policy);
    assert(total_charge >= meta_charge);
    return total_charge - meta_charge;
  }
};

// We provide our own simple hash table since it removes a whole bunch
// of porting hacks and is also faster than some of the built-in hash
// table implementations in some of the compiler/runtime combinations
// we have tested.  E.g., readrandom speeds up by ~5% over the g++
// 4.4.3's builtin hashtable.
class LRUHandleTable {
 public:
  explicit LRUHandleTable(int max_upper_hash_bits);
  ~LRUHandleTable();

  LRUHandle* Lookup(const Slice& key, uint32_t hash);
  LRUHandle* Insert(LRUHandle* h);
  LRUHandle* Remove(const Slice& key, uint32_t hash);

  template <typename T>
  void ApplyToEntriesRange(T func, size_t index_begin, size_t index_end) {
    for (size_t i = index_begin; i < index_end; i++) {
      LRUHandle* h = list_[i];
      while (h != nullptr) {
        auto n = h->next_hash;
        assert(h->InCache());
        func(h);
        h = n;
      }
    }
  }

  int GetLengthBits() const { return length_bits_; }

  size_t GetOccupancyCount() const { return elems_; }

 private:
  // Return a pointer to slot that points to a cache entry that
  // matches key/hash.  If there is no such cache entry, return a
  // pointer to the trailing slot in the corresponding linked list.
  LRUHandle** FindPointer(const Slice& key, uint32_t hash);

  void Resize();

  // Number of hash bits (upper because lower bits used for sharding)
  // used for table index. Length == 1 << length_bits_
  int length_bits_;

  // The table consists of an array of buckets where each bucket is
  // a linked list of cache entries that hash into the bucket.
  std::unique_ptr<LRUHandle*[]> list_;

  // Number of elements currently in the table.
  uint32_t elems_;

  // Set from max_upper_hash_bits (see constructor).
  const int max_length_bits_;
};

// A single shard of sharded cache.
class ALIGN_AS(CACHE_LINE_SIZE) LRUCacheShard final : public CacheShardBase {
 public:
  LRUCacheShard(size_t capacity, bool strict_capacity_limit,
                double high_pri_pool_ratio, double low_pri_pool_ratio,
                bool use_adaptive_mutex,
                CacheMetadataChargePolicy metadata_charge_policy,
                int max_upper_hash_bits, SecondaryCache* secondary_cache);

 public:  // Type definitions expected as parameter to ShardedCache
  using HandleImpl = LRUHandle;
  using HashVal = uint32_t;
  using HashCref = uint32_t;

 public:  // Function definitions expected as parameter to ShardedCache
  static inline HashVal ComputeHash(const Slice& key) {
    return Lower32of64(GetSliceNPHash64(key));
  }

  // Separate from constructor so caller can easily make an array of LRUCache
  // if current usage is more than new capacity, the function will attempt to
  // free the needed space.
  void SetCapacity(size_t capacity);

  // Set the flag to reject insertion if cache if full.
  void SetStrictCapacityLimit(bool strict_capacity_limit);

  // Set percentage of capacity reserved for high-pri cache entries.
  void SetHighPriorityPoolRatio(double high_pri_pool_ratio);

  // Set percentage of capacity reserved for low-pri cache entries.
  void SetLowPriorityPoolRatio(double low_pri_pool_ratio);

  // Like Cache methods, but with an extra "hash" parameter.
  inline Status Insert(const Slice& key, uint32_t hash, void* value,
                       size_t charge, Cache::DeleterFn deleter,
                       LRUHandle** handle, Cache::Priority priority) {
    return Insert(key, hash, value, charge, deleter, nullptr, handle, priority);
  }
  inline Status Insert(const Slice& key, uint32_t hash, void* value,
                       const Cache::CacheItemHelper* helper, size_t charge,
                       LRUHandle** handle, Cache::Priority priority) {
    assert(helper);
    return Insert(key, hash, value, charge, nullptr, helper, handle, priority);
  }
  // If helper_cb is null, the values of the following arguments don't matter.
  LRUHandle* Lookup(const Slice& key, uint32_t hash,
                    const Cache::CacheItemHelper* helper,
                    const Cache::CreateCallback& create_cb,
                    Cache::Priority priority, bool wait, Statistics* stats);
  inline LRUHandle* Lookup(const Slice& key, uint32_t hash) {
    return Lookup(key, hash, nullptr, nullptr, Cache::Priority::LOW, true,
                  nullptr);
  }
  bool Release(LRUHandle* handle, bool useful, bool erase_if_last_ref);
  bool IsReady(LRUHandle* /*handle*/);
  void Wait(LRUHandle* /*handle*/) {}
  bool Ref(LRUHandle* handle);
  void Erase(const Slice& key, uint32_t hash);

  // Although in some platforms the update of size_t is atomic, to make sure
  // GetUsage() and GetPinnedUsage() work correctly under any platform, we'll
  // protect them with mutex_.

  size_t GetUsage() const;
  size_t GetPinnedUsage() const;
  size_t GetOccupancyCount() const;
  size_t GetTableAddressCount() const;

  void ApplyToSomeEntries(
      const std::function<void(const Slice& key, void* value, size_t charge,
                               DeleterFn deleter)>& callback,
      size_t average_entries_per_lock, size_t* state);

  void EraseUnRefEntries();

 public:  // other function definitions
  void TEST_GetLRUList(LRUHandle** lru, LRUHandle** lru_low_pri,
                       LRUHandle** lru_bottom_pri);

  // Retrieves number of elements in LRU, for unit test purpose only.
  // Not threadsafe.
  size_t TEST_GetLRUSize();

  // Retrieves high pri pool ratio
  double GetHighPriPoolRatio();

  // Retrieves low pri pool ratio
  double GetLowPriPoolRatio();

  void AppendPrintableOptions(std::string& /*str*/) const;

 private:
  friend class LRUCache;
  // Insert an item into the hash table and, if handle is null, insert into
  // the LRU list. Older items are evicted as necessary. If the cache is full
  // and free_handle_on_fail is true, the item is deleted and handle is set to
  // nullptr.
  Status InsertItem(LRUHandle* item, LRUHandle** handle,
                    bool free_handle_on_fail);
  Status Insert(const Slice& key, uint32_t hash, void* value, size_t charge,
                DeleterFn deleter, const Cache::CacheItemHelper* helper,
                LRUHandle** handle, Cache::Priority priority);
  // Promote an item looked up from the secondary cache to the LRU cache.
  // The item may be still in the secondary cache.
  // It is only inserted into the hash table and not the LRU list, and only
  // if the cache is not at full capacity, as is the case during Insert.  The
  // caller should hold a reference on the LRUHandle. When the caller releases
  // the last reference, the item is added to the LRU list.
  // The item is promoted to the high pri or low pri pool as specified by the
  // caller in Lookup.
  void Promote(LRUHandle* e);
  void LRU_Remove(LRUHandle* e);
  void LRU_Insert(LRUHandle* e);

  // Overflow the last entry in high-pri pool to low-pri pool until size of
  // high-pri pool is no larger than the size specify by high_pri_pool_pct.
  void MaintainPoolSize();

  // Free some space following strict LRU policy until enough space
  // to hold (usage_ + charge) is freed or the lru list is empty
  // This function is not thread safe - it needs to be executed while
  // holding the mutex_.
  void EvictFromLRU(size_t charge, autovector<LRUHandle*>* deleted);

  // Try to insert the evicted handles into the secondary cache.
  void TryInsertIntoSecondaryCache(autovector<LRUHandle*> evicted_handles);

  // Initialized before use.
  size_t capacity_;

  // Memory size for entries in high-pri pool.
  size_t high_pri_pool_usage_;

  // Memory size for entries in low-pri pool.
  size_t low_pri_pool_usage_;

  // Whether to reject insertion if cache reaches its full capacity.
  bool strict_capacity_limit_;

  // Ratio of capacity reserved for high priority cache entries.
  double high_pri_pool_ratio_;

  // High-pri pool size, equals to capacity * high_pri_pool_ratio.
  // Remember the value to avoid recomputing each time.
  double high_pri_pool_capacity_;

  // Ratio of capacity reserved for low priority cache entries.
  double low_pri_pool_ratio_;

  // Low-pri pool size, equals to capacity * low_pri_pool_ratio.
  // Remember the value to avoid recomputing each time.
  double low_pri_pool_capacity_;

  // Dummy head of LRU list.
  // lru.prev is newest entry, lru.next is oldest entry.
  // LRU contains items which can be evicted, ie reference only by cache
  LRUHandle lru_;

  // Pointer to head of low-pri pool in LRU list.
  LRUHandle* lru_low_pri_;

  // Pointer to head of bottom-pri pool in LRU list.
  LRUHandle* lru_bottom_pri_;

  // ------------^^^^^^^^^^^^^-----------
  // Not frequently modified data members
  // ------------------------------------
  //
  // We separate data members that are updated frequently from the ones that
  // are not frequently updated so that they don't share the same cache line
  // which will lead into false cache sharing
  //
  // ------------------------------------
  // Frequently modified data members
  // ------------vvvvvvvvvvvvv-----------
  LRUHandleTable table_;

  // Memory size for entries residing in the cache.
  size_t usage_;

  // Memory size for entries residing only in the LRU list.
  size_t lru_usage_;

  // mutex_ protects the following state.
  // We don't count mutex_ as the cache's internal state so semantically we
  // don't mind mutex_ invoking the non-const actions.
  mutable DMutex mutex_;

  // Owned by LRUCache
  SecondaryCache* secondary_cache_;
};

class LRUCache
#ifdef NDEBUG
    final
#endif
    : public ShardedCache<LRUCacheShard> {
 public:
  LRUCache(size_t capacity, int num_shard_bits, bool strict_capacity_limit,
           double high_pri_pool_ratio, double low_pri_pool_ratio,
           std::shared_ptr<MemoryAllocator> memory_allocator = nullptr,
           bool use_adaptive_mutex = kDefaultToAdaptiveMutex,
           CacheMetadataChargePolicy metadata_charge_policy =
               kDontChargeCacheMetadata,
           std::shared_ptr<SecondaryCache> secondary_cache = nullptr);
  const char* Name() const override { return "LRUCache"; }
  void* Value(Handle* handle) override;
  size_t GetCharge(Handle* handle) const override;
  DeleterFn GetDeleter(Handle* handle) const override;
  void WaitAll(std::vector<Handle*>& handles) override;

  // Retrieves number of elements in LRU, for unit test purpose only.
  size_t TEST_GetLRUSize();
  // Retrieves high pri pool ratio.
  double GetHighPriPoolRatio();

  void AppendPrintableOptions(std::string& str) const override;

 private:
  std::shared_ptr<SecondaryCache> secondary_cache_;
};

}  // namespace lru_cache

using LRUCache = lru_cache::LRUCache;
using LRUHandle = lru_cache::LRUHandle;
using LRUCacheShard = lru_cache::LRUCacheShard;

}  // namespace ROCKSDB_NAMESPACE
