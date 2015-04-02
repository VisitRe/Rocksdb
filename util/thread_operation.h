// Copyright (c) 2013, Facebook, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.
//
// This file defines the structures for thread operation and state.
// Thread operations are used to describe high level action of a
// thread such as doing compaction or flush, while thread state
// are used to describe lower-level action such as reading /
// writing a file or waiting for a mutex.  Operations and states
// are designed to be independent.  Typically, a thread usually involves
// in one operation and one state at any specific point in time.

#pragma once

#include "include/rocksdb/thread_status.h"

#include <string>

namespace rocksdb {

#if ROCKSDB_USING_THREAD_STATUS

// The structure that describes a major thread operation.
struct OperationInfo {
  const ThreadStatus::OperationType type;
  const std::string name;
};

// The global operation table.
//
// When updating a status of a thread, the pointer of the OperationInfo
// of the current ThreadStatusData will be pointing to one of the
// rows in this global table.
//
// Note that it's not designed to be constant as in the future we
// might consider adding global count to the OperationInfo.
static OperationInfo global_operation_table[] = {
  {ThreadStatus::OP_UNKNOWN, ""},
  {ThreadStatus::OP_COMPACTION, "Compaction"},
  {ThreadStatus::OP_FLUSH, "Flush"}
};

// The structure that describes a state.
struct StateInfo {
  const ThreadStatus::StateType type;
  const std::string name;
};

// The global state table.
//
// When updating a status of a thread, the pointer of the StateInfo
// of the current ThreadStatusData will be pointing to one of the
// rows in this global table.
static StateInfo global_state_table[] = {
  {ThreadStatus::STATE_UNKNOWN, ""},
  {ThreadStatus::STATE_MUTEX_WAIT, "Mutex Wait"},
};

#else

struct OperationInfo {
};

struct StateInfo {
};

#endif  // ROCKSDB_USING_THREAD_STATUS
}  // namespace rocksdb
