//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include "rocksdb/iterator.h"
#include "rocksdb/options.h"
#include "rocksdb/rocksdb_namespace.h"
#include "rocksdb/status.h"
#include "rocksdb/wide_columns.h"

namespace ROCKSDB_NAMESPACE {

class Iterator;
class ColumnFamilyHandle;
struct ReadOptions;

// UNDER CONSTRUCTION - DO NOT USE
// A cross-column-family iterator from a consistent database state.
// When the same key exists in more than one column families, the iterator
// selects the value from the first column family containing the key, in the
// order provided in the `column_families` parameter.
class MultiCfIterator : public Iterator {
 public:
  MultiCfIterator() {}
  virtual ~MultiCfIterator() {}

  // No copy allowed
  MultiCfIterator(const MultiCfIterator&) = delete;
  MultiCfIterator& operator=(const MultiCfIterator&) = delete;

  virtual const AttributeGroups& attribute_groups() const = 0;
};
extern MultiCfIterator* NewMultiColumnFamilyIterator(
    const Comparator* comparator,
    const std::vector<ColumnFamilyHandle*>& column_families,
    const std::vector<Iterator*>& child_iterators);

// Return an empty MultiCfIterator (yields nothing)
extern MultiCfIterator* NewEmptyMultiColumnFamilyIterator();

// Return an empty MultiCfIterator with the specified status.
extern MultiCfIterator* NewErrorMultiColumnFamilyIterator(const Status& status);

}  // namespace ROCKSDB_NAMESPACE
