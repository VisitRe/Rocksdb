// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// This file implements the "bridge" between Java and C++ for
// ROCKSDB_NAMESPACE::ClockCache.

#include "cache/clock_cache.h"

#include <jni.h>

#include "include/org_rocksdb_ClockCache.h"
#include "rocksjni/cplusplus_to_java_convert.h"

/*
 * Class:     org_rocksdb_ClockCache
 * Method:    newClockCache
 * Signature: (JIZ)J
 */
jlong Java_org_rocksdb_ClockCache_newClockCache(
    JNIEnv* /*env*/, jclass /*jcls*/, jlong jcapacity,
    jlong jestimated_value_size, jint jnum_shard_bits,
    jboolean jstrict_capacity_limit) {
  auto* sptr_clock_cache = new std::shared_ptr<ROCKSDB_NAMESPACE::Cache>(
      ROCKSDB_NAMESPACE::NewClockCache(
          static_cast<size_t>(jcapacity),
          static_cast<size_t>(jestimated_value_size),
          static_cast<int>(jnum_shard_bits),
          static_cast<bool>(jstrict_capacity_limit),
      rocksdb::kDefaultCacheMetadataChargePolicy));
  return GET_CPLUSPLUS_POINTER(sptr_clock_cache);
}

/*
 * Class:     org_rocksdb_ClockCache
 * Method:    disposeInternal
 * Signature: (J)V
 */
void Java_org_rocksdb_ClockCache_disposeInternal(JNIEnv* /*env*/,
                                                 jobject /*jobj*/,
                                                 jlong jhandle) {
  auto* sptr_clock_cache =
      reinterpret_cast<std::shared_ptr<ROCKSDB_NAMESPACE::Cache>*>(jhandle);
  delete sptr_clock_cache;  // delete std::shared_ptr
}
