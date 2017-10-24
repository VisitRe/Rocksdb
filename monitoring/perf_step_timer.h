//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
#pragma once
#include "monitoring/perf_level_imp.h"
#include "rocksdb/env.h"
#include "util/stop_watch.h"

namespace rocksdb {

class PerfStepTimer {
 public:
  explicit PerfStepTimer(uint64_t* metric, bool for_mutex = false)
      : enabled_(perf_level >= PerfLevel::kEnableTime ||
                 (!for_mutex && perf_level >= kEnableTimeExceptForMutex)),
        env_(enabled_ ? Env::Default() : nullptr),
        start_(0),
        metric_(metric) {}

  ~PerfStepTimer() {
    Stop();
  }

  void Start() {
    if (enabled_) {
      start_ = env_->NowNanos();
    }
  }

  void Measure() {
    if (start_) {
      uint64_t now = env_->NowNanos();
      *metric_ += now - start_;
      start_ = now;
    }
  }

  void Stop() {
    if (start_) {
      *metric_ += env_->NowNanos() - start_;
      start_ = 0;
    }
  }

 private:
  const bool enabled_;
  Env* const env_;
  uint64_t start_;
  uint64_t* metric_;
};

// This class does not capture the pointer
// to the actual metric because the completion
// may happen in a different thread.
// It accesses the metric at the reporting time
// Measure makes it possible to charge part of the
// time to one thread and the reminder to another
class PerfMeter {
public:
  explicit
  PerfMeter(bool for_mutex = false) :
    enabled_(perf_level >= PerfLevel::kEnableTime ||
    (!for_mutex && perf_level >= kEnableTimeExceptForMutex)),
    env_(enabled_ ? Env::Default() : nullptr),
    start_(0) {
  }

  void Start() {
    if (enabled_) {
      start_ = env_->NowNanos();
    }
  }

  void Measure(uint64_t* metric) {
    if (start_) {
      uint64_t now = env_->NowNanos();
      *metric += now - start_;
      start_ = now;
    }
  }

  void Stop(uint64_t* metric) {
    if (start_) {
      *metric += env_->NowNanos() - start_;
      start_ = 0;
    }
  }

private:
  const bool enabled_;
  Env* const env_;
  uint64_t start_;
};

}  // namespace rocksdb
