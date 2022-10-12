//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
#pragma once
#include "rocksdb/perf_level.h"
#include "port/port.h"

namespace rocksdb {

inline PerfLevel* get_perf_level() {
  static ThreadLocal<PerfLevel> pl(kEnableCount);
  return &pl;
}

#ifdef ROCKSDB_SUPPORT_THREAD_LOCAL
#else
extern PerfLevel perf_level;
#endif

}  // namespace rocksdb
