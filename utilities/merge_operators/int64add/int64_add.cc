// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "utilities/merge_operators/int64add/int64_add.h"

#include <memory>

#include "logging/logging.h"
#include "rocksdb/env.h"
#include "rocksdb/merge_operator.h"
#include "rocksdb/slice.h"
#include "util/coding.h"
#include "utilities/merge_operators.h"

namespace ROCKSDB_NAMESPACE {

// A 'model' merge operator with int64 addition semantics
// operands and database value should be variable length encoded
// int64_t values, as encoded/decoded by `util/coding.h`.
bool Int64AddOperator::Merge(const Slice&, const Slice* existing_value,
                             const Slice& value, std::string* new_value,
                             Logger* /*logger*/) const {
  int64_t orig_value = 0;
  if (existing_value) {
    Slice ev(*existing_value);
    orig_value = Get8BitVarsignedint64(&ev);
  }

  Slice v(value);
  int64_t operand = Get8BitVarsignedint64(&v);

  assert(new_value);
  new_value->clear();
  const int64_t new_number = orig_value + operand;
  ROCKSDB_NAMESPACE::Put8BitVarsignedint64(new_value, new_number);

  return true;
}

  std::shared_ptr<MergeOperator> MergeOperators::CreateInt64AddOperator() {
    return std::make_shared<Int64AddOperator>();
  }

}; // namespace ROCKSDB_NAMESPACE
