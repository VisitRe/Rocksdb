//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "rocksdb/wide_columns.h"

#include <algorithm>

#include "db/wide/wide_column_serialization.h"

namespace ROCKSDB_NAMESPACE {

const Slice kDefaultWideColumnName;

const WideColumns kNoWideColumns;

Status PinnableWideColumns::SetFromWideColumns(WideColumns columns) {
  std::sort(columns.begin(), columns.end(),
            [](const WideColumn& lhs, const WideColumn& rhs) {
              return lhs.name().compare(rhs.name()) < 0;
            });

  const Status s =
      WideColumnSerialization::Serialize(columns, *value_.GetSelf());
  if (!s.ok()) {
    return s;
  }

  return CreateIndexForWideColumns();
}

Status PinnableWideColumns::CreateIndexForWideColumns() {
  Slice value_copy = value_;

  return WideColumnSerialization::Deserialize(value_copy, columns_);
}

}  // namespace ROCKSDB_NAMESPACE
