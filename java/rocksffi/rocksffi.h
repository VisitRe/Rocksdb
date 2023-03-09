// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// This file implements the "bridge" between Java and C++ and enables
// calling c++ ROCKSDB_NAMESPACE::DB methods from Java side.

#pragma once

#include <stddef.h>

#include <cstddef>
#include <string>

#include "rocksdb/db.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"

typedef struct rocksdb_input_slice {
  const char* data;
  size_t size;
} rocksdb_input_slice_t;

typedef struct rocksdb_output_slice {
  const char* data;
  size_t size;
  ROCKSDB_NAMESPACE::PinnableSlice* pinnable_slice;
} rocksdb_output_slice_t;

extern "C" int rocksdb_ffi_get(ROCKSDB_NAMESPACE::DB* db,
                               ROCKSDB_NAMESPACE::ColumnFamilyHandle* cf,
                               rocksdb_input_slice_t* key,
                               rocksdb_output_slice_t* value);

extern "C" int rocksdb_ffi_reset_output(rocksdb_output_slice_t* value);