//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.


#pragma once
#ifndef ROCKSDB_LITE

#include "rocksdb/utilities/transaction_db_mutex.h"

namespace rocksdb {

class TransactionDBMutex;
class TransactionDBCondVar;

// Default implementation of TransactionDBMutexFactory.  May be overridden
// by TransactionDBOptions.custom_mutex_factory.
class TransactionDBMutexFactoryImpl : public TransactionDBMutexFactory {
 public:
  std::shared_ptr<TransactionDBMutex> AllocateMutex() override;
  std::shared_ptr<TransactionDBCondVar> AllocateCondVar() override;
};

}  //  namespace rocksdb

#endif  // ROCKSDB_LITE
