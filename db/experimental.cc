//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.


#include "rocksdb/experimental.h"

#include "db/db_impl.h"

namespace rocksdb {
namespace experimental {

#ifndef ROCKSDB_LITE

Status SuggestCompactRange(DB* db, ColumnFamilyHandle* column_family,
                           const Slice* begin, const Slice* end) {
  auto dbimpl = dynamic_cast<DBImpl*>(db);
  if (dbimpl == nullptr) {
    return Status::InvalidArgument("Didn't recognize DB object");
  }

  return dbimpl->SuggestCompactRange(column_family, begin, end);
}

Status PromoteL0(DB* db, ColumnFamilyHandle* column_family, int target_level) {
  auto dbimpl = dynamic_cast<DBImpl*>(db);
  if (dbimpl == nullptr) {
    return Status::InvalidArgument("Didn't recognize DB object");
  }
  return dbimpl->PromoteL0(column_family, target_level);
}

#else  // ROCKSDB_LITE

Status SuggestCompactRange(DB* db, ColumnFamilyHandle* column_family,
                           const Slice* begin, const Slice* end) {
  return Status::NotSupported("Not supported in RocksDB LITE");
}

Status PromoteL0(DB* db, ColumnFamilyHandle* column_family, int target_level) {
  return Status::NotSupported("Not supported in RocksDB LITE");
}

#endif  // ROCKSDB_LITE

Status SuggestCompactRange(DB* db, const Slice* begin, const Slice* end) {
  return SuggestCompactRange(db, db->DefaultColumnFamily(), begin, end);
}

}  // namespace experimental
}  // namespace rocksdb
