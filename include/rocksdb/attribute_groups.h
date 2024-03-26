//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <ostream>
#include <tuple>
#include <utility>
#include <vector>

#include "rocksdb/comparator.h"
#include "rocksdb/iterator_base.h"
#include "rocksdb/rocksdb_namespace.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "rocksdb/wide_columns.h"

namespace ROCKSDB_NAMESPACE {

class ColumnFamilyHandle;

// Class representing attribute group. Attribute group is a logical grouping of
// wide-column entities by leveraging Column Families.
// Used in Write Path
class AttributeGroup {
 public:
  ColumnFamilyHandle* column_family() const { return column_family_; }
  const WideColumns& columns() const { return columns_; }
  WideColumns& columns() { return columns_; }

  explicit AttributeGroup(ColumnFamilyHandle* column_family,
                          const WideColumns& columns)
      : column_family_(column_family), columns_(columns) {}

 private:
  ColumnFamilyHandle* column_family_;
  WideColumns columns_;
};

inline bool operator==(const AttributeGroup& lhs, const AttributeGroup& rhs) {
  return lhs.column_family() == rhs.column_family() &&
         lhs.columns() == rhs.columns();
}

// A collection of Attribute Groups.
using AttributeGroups = std::vector<AttributeGroup>;

// An empty set of Attribute Groups.
extern const AttributeGroups kNoAttributeGroups;

// Used in Read Path. Wide-columns returned from the query are pinnable.
class PinnableAttributeGroup {
 public:
  ColumnFamilyHandle* column_family() const { return column_family_; }
  const Status& status() const { return status_; }
  const WideColumns& columns() const { return columns_.columns(); }

  explicit PinnableAttributeGroup(ColumnFamilyHandle* column_family)
      : column_family_(column_family), status_(Status::OK()) {}

  void SetStatus(const Status& status);
  void SetColumns(PinnableWideColumns&& columns);

  void Reset();

 private:
  ColumnFamilyHandle* column_family_;
  Status status_;
  PinnableWideColumns columns_;
};

inline void PinnableAttributeGroup::SetStatus(const Status& status) {
  status_ = status;
}
inline void PinnableAttributeGroup::SetColumns(PinnableWideColumns&& columns) {
  columns_ = std::move(columns);
}

inline void PinnableAttributeGroup::Reset() {
  SetStatus(Status::OK());
  columns_.Reset();
}

// A collection of Pinnable Attribute Groups.
using PinnableAttributeGroups = std::vector<PinnableAttributeGroup>;

// UNDER CONSTRUCTION - DO NOT USE
// A cross-column-family iterator that collects and returns attribute groups for
// each key in order provided by comparator
class AttributeGroupIterator : public IteratorBase {
 public:
  AttributeGroupIterator() {}
  ~AttributeGroupIterator() override {}

  // No copy allowed
  AttributeGroupIterator(const AttributeGroupIterator&) = delete;
  AttributeGroupIterator& operator=(const AttributeGroupIterator&) = delete;

  virtual AttributeGroups attribute_groups() const = 0;
};

}  // namespace ROCKSDB_NAMESPACE
