//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//  This source code is also licensed under the GPLv2 license found in the
//  COPYING file in the root directory of this source tree.

#pragma once
#ifndef ROCKSDB_LITE

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/utilities/optimistic_transaction_db.h"

namespace rocksdb {

class OptimisticTransactionDBImpl : public OptimisticTransactionDB {
 public:
  explicit OptimisticTransactionDBImpl(DB* db)
      : OptimisticTransactionDB(db), db_(db) {}

  ~OptimisticTransactionDBImpl() {}

  Transaction* BeginTransaction(const WriteOptions& write_options,
                                const OptimisticTransactionOptions& txn_options,
                                Transaction* old_txn) override;

  DB* GetBaseDB() override { return db_.get(); }

 private:
  std::unique_ptr<DB> db_;

  void ReinitializeTransaction(Transaction* txn,
                               const WriteOptions& write_options,
                               const OptimisticTransactionOptions& txn_options =
                                   OptimisticTransactionOptions());
};

}  //  namespace rocksdb
#endif  // ROCKSDB_LITE
