// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// This file defines the "bridge" object between Java and C++ for
// ROCKSDB_NAMESPACE::ColumnFamilyHandle.

#pragma once

#include <jni.h>

#include <iostream>

#include "api_rocksdb.h"
#include "api_weakdb.h"
#include "portal.h"
#include "rocksdb/db.h"

template <class TDatabase>
class APIColumnFamilyHandle : public APIWeakDB<TDatabase> {
 public:
  std::weak_ptr<ROCKSDB_NAMESPACE::ColumnFamilyHandle> cfh;

  APIColumnFamilyHandle(
      std::shared_ptr<TDatabase>& db,
      std::shared_ptr<ROCKSDB_NAMESPACE::ColumnFamilyHandle>& cfh)
      : APIWeakDB<TDatabase>(db), cfh(cfh){};

  /**
   * @brief lock the CF (std::shared_ptr) if the weak pointer is valid
   * @return locked CF if the weak ptr is still valid
   */
  const std::shared_ptr<ROCKSDB_NAMESPACE::ColumnFamilyHandle> cfhLock(
      JNIEnv* env) const {
    auto lock = cfh.lock();
    if (!lock) {
      ROCKSDB_NAMESPACE::RocksDBExceptionJni::ThrowNew(
          env, ROCKSDB_NAMESPACE::RocksDBExceptionJni::OrphanedColumnFamily());
    }
    return lock;
  }

  /**
   * @brief lock the CF (std::shared_ptr) if the weak pointer is valid, and
   * check we have the correct DB. This check fails erroneusly if used by a
   * wrapper for a non-standard database e.g. open a CF with an optimistic
   * transaction DB, use it in the context of the base DB, error...
   *
   * TODO (AP)
   * While it is possible to add complexity to check the correct _base_ DB,
   * maybe this is best left until a bit more testing is done ?
   *
   * @return locked CF if the weak ptr is still valid and the DB matches, empty
   * ptr otherwise
   */
  std::shared_ptr<ROCKSDB_NAMESPACE::ColumnFamilyHandle> cfhLockDBCheck(
      JNIEnv* env, APIRocksDB<TDatabase>& dbAPI) {
    if (this->dbLock(env) != *dbAPI) {
      ROCKSDB_NAMESPACE::RocksDBExceptionJni::ThrowNew(
          env,
          ROCKSDB_NAMESPACE::RocksDBExceptionJni::MismatchedColumnFamily());
      std::shared_ptr<ROCKSDB_NAMESPACE::ColumnFamilyHandle> lock;
      return lock;
    }
    auto lock = cfh.lock();
    if (!lock) {
      ROCKSDB_NAMESPACE::RocksDBExceptionJni::ThrowNew(
          env, ROCKSDB_NAMESPACE::RocksDBExceptionJni::OrphanedColumnFamily());
    }
    return lock;
  }

  /**
   * @brief lock the referenced pointer if the weak pointer is valid
   *
   * @param handle
   * @return std::shared_ptr<ROCKSDB_NAMESPACE::ColumnFamilyHandle>
   * There is an exception raised iff !cfh
   */
  static std::shared_ptr<ROCKSDB_NAMESPACE::ColumnFamilyHandle> lock(
      JNIEnv* env, jlong handle) {
    auto* cfhAPI = reinterpret_cast<APIColumnFamilyHandle*>(handle);
    std::shared_ptr<ROCKSDB_NAMESPACE::ColumnFamilyHandle> cfh;
    if (cfhAPI == nullptr) {
      ROCKSDB_NAMESPACE::RocksDBExceptionJni::ThrowNew(
          env, ROCKSDB_NAMESPACE::RocksDBExceptionJni::InvalidColumnFamily());
      return cfh;
    }
    cfh = cfhAPI->cfh.lock();
    if (!cfh) {
      ROCKSDB_NAMESPACE::RocksDBExceptionJni::ThrowNew(
          env, "Column family already closed");
    }
    return cfh;
  }

  /**
   * @brief lock an array of reference pointers
   *
   * @param env
   * @param jhandles
   * @return std::vector<std::shared_ptr<ROCKSDB_NAMESPACE::ColumnFamilyHandle>>
   * a vector of locked (shared_ptr) CF handles
   */
  static std::vector<std::shared_ptr<ROCKSDB_NAMESPACE::ColumnFamilyHandle>>
  lock(JNIEnv* env, jlongArray jhandles, jboolean* has_exception) {
    const jsize jhandles_len = env->GetArrayLength(jhandles);
    std::vector<std::shared_ptr<ROCKSDB_NAMESPACE::ColumnFamilyHandle>> handles;
    jlong* jhandle = env->GetLongArrayElements(jhandles, nullptr);
    if (jhandle == nullptr) {
      // exception thrown: OutOfMemoryError
      *has_exception = JNI_TRUE;
      return handles;
    }
    handles.reserve(jhandles_len);
    for (jsize i = 0; i < jhandles_len; i++) {
      auto* cfhAPI = reinterpret_cast<APIColumnFamilyHandle*>(jhandle[i]);
      if (cfhAPI == nullptr) {
        ROCKSDB_NAMESPACE::RocksDBExceptionJni::ThrowNew(
            env, ROCKSDB_NAMESPACE::RocksDBExceptionJni::InvalidColumnFamily());
        *has_exception = JNI_TRUE;
        return handles;
      }
      std::shared_ptr<ROCKSDB_NAMESPACE::ColumnFamilyHandle> cfh =
          cfhAPI->cfh.lock();
      if (!cfh) {
        ROCKSDB_NAMESPACE::RocksDBExceptionJni::ThrowNew(
            env, "Column family already closed");
        *has_exception = JNI_TRUE;
        return handles;
      }
      handles.push_back(cfh);
    }
    return handles;
  }

  static std::shared_ptr<ROCKSDB_NAMESPACE::ColumnFamilyHandle>
  lockCFHOrDefault(JNIEnv* env, jlong jhandle,
                   const APIRocksDB<TDatabase>& dbAPI) {
    if (jhandle != 0) {
      return lock(env, jhandle);
    } else {
      auto defaultHandle = dbAPI.defaultColumnFamilyHandle;
      if (!defaultHandle) {
        ROCKSDB_NAMESPACE::RocksDBExceptionJni::ThrowNew(
            env, "Default column family is closed. DB may already be closed.");
      }
      return defaultHandle;
    }
  }

  std::vector<long> use_counts() {
    std::vector<long> vec;

    auto dbLocked = this->db.lock();
    auto cfhLocked = cfh.lock();
    if (dbLocked) {
      vec.push_back(dbLocked.use_count());
    } else {
      vec.push_back(0);
    }
    if (cfhLocked) {
      vec.push_back(cfhLocked.use_count());
    } else {
      vec.push_back(0);
    }

    return vec;
  }
};
