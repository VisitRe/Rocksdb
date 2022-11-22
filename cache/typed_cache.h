//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "cache/cache_helpers.h"
#include "rocksdb/advanced_options.h"
#include "rocksdb/cache.h"

namespace ROCKSDB_NAMESPACE {

// For future consideration:
// * Pass in value to Insert with std::unique_ptr& to simplify ownership
//   transfer logic in callers
// * Make key type a template parameter (e.g. useful for table cache)
// * Closer integration with CacheHandleGuard (opt-in, so not always
//   paying the extra overhead)

#define CACHE_TYPE_DEFS()                     \
  using Priority = Cache::Priority;           \
  using Handle = Cache::Handle;               \
  using ValueType = Cache::ValueType;         \
  using CreateContext = Cache::CreateContext; \
  using CacheItemHelper = Cache::CacheItemHelper /* caller ; */

template <typename CachePtr>
class BaseCacheInterface {
 public:
  CACHE_TYPE_DEFS();

  BaseCacheInterface(CachePtr cache) : cache_(std::move(cache)) {}

  inline void Release(Handle* handle) { cache_->Release(handle); }

  inline void ReleaseAndEraseIfLastRef(Handle* handle) {
    cache_->Release(handle, /*erase_if_last_ref*/ true);
  }

  inline void RegisterReleaseAsCleanup(Handle* handle, Cleanable& cleanable) {
    cleanable.RegisterCleanup(&ReleaseCacheHandleCleanup, get(), handle);
  }

  inline Cache* get() const { return &*cache_; }

  explicit inline operator bool() const noexcept { return cache_ != nullptr; }

 protected:
  CachePtr cache_;
};

template <CacheEntryRole kRole, typename CachePtr = Cache*>
class PlaceholderCacheInterface : public BaseCacheInterface<CachePtr> {
 public:
  CACHE_TYPE_DEFS();
  using BaseCacheInterface<CachePtr>::BaseCacheInterface;

  inline Status Insert(const Slice& key, size_t charge, Handle** handle) {
    return this->cache_->Insert(key, /*value=*/nullptr, &kHelper, charge,
                                handle);
  }

  static constexpr Cache::CacheItemHelper kHelper{kRole};
};

template <CacheEntryRole kRole>
using PlaceholderSharedCacheInterface =
    PlaceholderCacheInterface<kRole, std::shared_ptr<Cache>>;

template <class TValue>
class BasicTypedCacheHelperFns {
 public:
  CACHE_TYPE_DEFS();
  // E.g. char* for char[]
  using TValuePtr = std::remove_extent_t<TValue>*;

 protected:
  inline static ValueType* UpCastValue(TValuePtr value) {
    // We don't strictly require values to extend ValueType, but it's a
    // good idea
    if constexpr (std::is_convertible_v<TValuePtr, ValueType*>) {
      return value;
    } else {
      return reinterpret_cast<ValueType*>(value);
    }
  }
  inline static TValuePtr DownCastValue(ValueType* value) {
    if constexpr (std::is_convertible_v<TValuePtr, ValueType*>) {
      return static_cast<TValuePtr>(value);
    } else {
      return reinterpret_cast<TValuePtr>(value);
    }
  }

  static void Delete(ValueType* value, MemoryAllocator* allocator) {
    // FIXME: Currently, no callers actually allocate the ValueType objects
    // using the custom allocator, just subobjects that keep a reference to
    // the allocator themselves (with CacheAllocationPtr).
    if (/*DISABLED*/ false && allocator) {
      if constexpr (std::is_destructible_v<TValue>) {
        DownCastValue(value)->~TValue();
      }
      allocator->Deallocate(value);
    } else {
      // Like delete but properly handles ValueType=char[] etc.
      std::default_delete<TValue>{}(DownCastValue(value));
    }
  }
};

// In its own class to try to minimize the number of distinct CacheItemHelper
// instances (e.g. don't vary by CachePtr)
template <class TValue, CacheEntryRole kRole>
class BasicTypedCacheHelper : public BasicTypedCacheHelperFns<TValue> {
 public:
  static constexpr Cache::CacheItemHelper kBasicHelper{
      kRole, &BasicTypedCacheHelper::Delete};
};

template <class TValue, CacheEntryRole kRole = TValue::kCacheEntryRole,
          typename CachePtr = Cache*>
class BasicTypedCacheInterface : public BaseCacheInterface<CachePtr>,
                                 public BasicTypedCacheHelper<TValue, kRole> {
 public:
  CACHE_TYPE_DEFS();
  using typename BasicTypedCacheHelperFns<TValue>::TValuePtr;
  struct TypedHandle : public Handle {};
  using BasicTypedCacheHelper<TValue, kRole>::kBasicHelper;
  // ctor
  using BaseCacheInterface<CachePtr>::BaseCacheInterface;

  inline Status Insert(const Slice& key, TValuePtr value, size_t charge,
                       TypedHandle** handle = nullptr,
                       Priority priority = Priority::LOW) {
    auto untyped_handle = reinterpret_cast<Handle**>(handle);
    return this->cache_->Insert(
        key, BasicTypedCacheHelperFns<TValue>::UpCastValue(value),
        &kBasicHelper, charge, untyped_handle, priority);
  }

  inline TypedHandle* Lookup(const Slice& key, Statistics* stats = nullptr) {
    return reinterpret_cast<TypedHandle*>(
        this->cache_->BasicLookup(key, stats));
  }

  inline CacheHandleGuard<TValue> Guard(TypedHandle* handle) {
    if (handle) {
      return CacheHandleGuard<TValue>(&*this->cache_, handle);
    } else {
      return {};
    }
  }

  inline std::shared_ptr<TValue> SharedGuard(TypedHandle* handle) {
    if (handle) {
      return MakeSharedCacheHandleGuard<TValue>(&*this->cache_, handle);
    } else {
      return {};
    }
  }

  inline TValuePtr Value(TypedHandle* handle) {
    return BasicTypedCacheHelperFns<TValue>::DownCastValue(
        this->cache_->Value(handle));
  }
};

template <class TValue, CacheEntryRole kRole = TValue::kCacheEntryRole>
using BasicTypedSharedCacheInterface =
    BasicTypedCacheInterface<TValue, kRole, std::shared_ptr<Cache>>;

// TValue must implement ContentSlice() and ~TValue
// TCreateContext must implement Create(std::unique_ptr<TValue>*, ...)
template <class TValue, class TCreateContext>
class FullTypedCacheHelperFns : public BasicTypedCacheHelperFns<TValue> {
 public:
  CACHE_TYPE_DEFS();

 protected:
  using typename BasicTypedCacheHelperFns<TValue>::TValuePtr;
  using BasicTypedCacheHelperFns<TValue>::DownCastValue;
  using BasicTypedCacheHelperFns<TValue>::UpCastValue;

  static size_t Size(ValueType* v) {
    TValuePtr value = DownCastValue(v);
    auto slice = value->ContentSlice();
    return slice.size();
  }

  static Status SaveTo(ValueType* v, size_t from_offset, size_t length,
                       char* out) {
    TValuePtr value = DownCastValue(v);
    auto slice = value->ContentSlice();
    assert(from_offset < slice.size());
    assert(from_offset + length <= slice.size());
    std::copy_n(slice.data() + from_offset, length, out);
    return Status::OK();
  }

  static Status Create(const Slice& data, CreateContext* context,
                       MemoryAllocator* allocator, ValueType** out_obj,
                       size_t* out_charge) {
    std::unique_ptr<TValue> value = nullptr;
    if constexpr (sizeof(TCreateContext) > 0) {
      TCreateContext* tcontext = static_cast<TCreateContext*>(context);
      tcontext->Create(&value, out_charge, data, allocator);
    } else {
      TCreateContext::Create(&value, out_charge, data, allocator);
    }
    *out_obj = UpCastValue(value.release());
    return Status::OK();
  }
};

// In its own class to try to minimize the number of distinct CacheItemHelper
// instances (e.g. don't vary by CachePtr)
template <class TValue, class TCreateContext, CacheEntryRole kRole>
class FullTypedCacheHelper
    : public FullTypedCacheHelperFns<TValue, TCreateContext> {
 public:
  static constexpr Cache::CacheItemHelper kFullHelper{
      kRole, &FullTypedCacheHelper::Delete, &FullTypedCacheHelper::Size,
      &FullTypedCacheHelper::SaveTo, &FullTypedCacheHelper::Create};
};

template <class TValue, class TCreateContext,
          CacheEntryRole kRole = TValue::kCacheEntryRole,
          typename CachePtr = Cache*>
class FullTypedCacheInterface
    : public BasicTypedCacheInterface<TValue, kRole, CachePtr>,
      public FullTypedCacheHelper<TValue, TCreateContext, kRole> {
 public:
  CACHE_TYPE_DEFS();
  using typename BasicTypedCacheInterface<TValue, kRole, CachePtr>::TypedHandle;
  using typename BasicTypedCacheHelperFns<TValue>::TValuePtr;
  using BasicTypedCacheHelper<TValue, kRole>::kBasicHelper;
  using FullTypedCacheHelper<TValue, TCreateContext, kRole>::kFullHelper;
  using BasicTypedCacheHelperFns<TValue>::UpCastValue;
  using BasicTypedCacheHelperFns<TValue>::DownCastValue;
  // ctor
  using BasicTypedCacheInterface<TValue, kRole,
                                 CachePtr>::BasicTypedCacheInterface;

  inline Status InsertFull(
      const Slice& key, TValuePtr value, size_t charge,
      TypedHandle** handle = nullptr, Priority priority = Priority::LOW,
      CacheTier lowest_used_cache_tier = CacheTier::kNonVolatileBlockTier) {
    auto untyped_handle = reinterpret_cast<Handle**>(handle);
    auto helper = lowest_used_cache_tier == CacheTier::kNonVolatileBlockTier
                      ? &kFullHelper
                      : &kBasicHelper;
    return this->cache_->Insert(key, UpCastValue(value), helper, charge,
                                untyped_handle, priority);
  }

  inline Status Warm(
      const Slice& key, const Slice& data, TCreateContext* create_context,
      Priority priority = Priority::LOW,
      CacheTier lowest_used_cache_tier = CacheTier::kNonVolatileBlockTier,
      size_t* out_charge = nullptr) {
    ValueType* value;
    size_t charge;
    Status st = kFullHelper.create_cb(data, create_context,
                                      this->cache_->memory_allocator(), &value,
                                      &charge);
    if (out_charge) {
      *out_charge = charge;
    }
    if (st.ok()) {
      st = InsertFull(key, DownCastValue(value), charge, nullptr /*handle*/,
                      priority, lowest_used_cache_tier);
    } else {
      kFullHelper.del_cb(value, this->cache_->memory_allocator());
    }
    return st;
  }

  inline TypedHandle* LookupFull(
      const Slice& key, TCreateContext* create_context = nullptr,
      Priority priority = Priority::LOW, bool wait = true,
      Statistics* stats = nullptr,
      CacheTier lowest_used_cache_tier = CacheTier::kNonVolatileBlockTier) {
    if (lowest_used_cache_tier == CacheTier::kNonVolatileBlockTier) {
      return reinterpret_cast<TypedHandle*>(this->cache_->Lookup(
          key, &kFullHelper, create_context, priority, wait, stats));
    } else {
      return BasicTypedCacheInterface<TValue, kRole, CachePtr>::Lookup(key,
                                                                       stats);
    }
  }
};

template <class TValue, class TCreateContext,
          CacheEntryRole kRole = TValue::kCacheEntryRole>
using FullTypedSharedCacheInterface =
    FullTypedCacheInterface<TValue, TCreateContext, kRole,
                            std::shared_ptr<Cache>>;

#undef CACHE_TYPE_DEFS

}  // namespace ROCKSDB_NAMESPACE
