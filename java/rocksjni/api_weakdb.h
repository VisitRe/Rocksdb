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

#include "api_base.h"
#include "portal.h"
#include "rocksdb/db.h"

class APIWeakDB : APIBase {
 public:
  std::weak_ptr<ROCKSDB_NAMESPACE::DB> db;

  APIWeakDB(std::shared_ptr<ROCKSDB_NAMESPACE::DB> db) : db(db){};

  /**
   * @brief lock the referenced pointer if the weak pointer is valid
   *
   * @return std::shared_ptr<ROCKSDB_NAMESPACE::ColumnFamilyHandle>
   */
  std::shared_ptr<ROCKSDB_NAMESPACE::DB> dbLock(JNIEnv* env) {
    auto lock = db.lock();
    if (!lock) {
      ROCKSDB_NAMESPACE::RocksDBExceptionJni::ThrowNew(
          env, "Column family (DB) already closed");
    }
    return lock;
  }

  /**
   * @brief lock the referenced pointer if the weak pointer is valid
   *
   * @param handle
   * @return std::shared_ptr<ROCKSDB_NAMESPACE::ColumnFamilyHandle>
   */
  static std::shared_ptr<ROCKSDB_NAMESPACE::DB> lockDB(JNIEnv* env,
                                                       jlong handle);
};
