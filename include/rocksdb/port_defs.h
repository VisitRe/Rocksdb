//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// This file includes the common definitions used in the port/,
// the public API (this directory), and other directories

#pragma once

#include "rocksdb/rocksdb_namespace.h"

namespace ROCKSDB_NAMESPACE {

enum class CpuPriority {
  kIdle = 0,
  kLow = 1,
  kNormal = 2,
  kHigh = 3,
};

class MutexBase {
 public:
  virtual ~MutexBase() {}

  virtual void Lock() = 0;
  virtual void Unlock() = 0;
};

class CondVarBase {
 public:
  virtual ~CondVarBase() {}

  virtual MutexBase* GetMutex() const = 0;

  virtual void Wait() = 0;
  // Timed condition wait.  Returns true if timeout occurred.
  virtual bool TimedWait(uint64_t abs_time_us) = 0;
  virtual void Signal() = 0;
  virtual void SignalAll() = 0;
};

}  // namespace ROCKSDB_NAMESPACE
