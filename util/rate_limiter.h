//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//  This source code is also licensed under the GPLv2 license found in the
//  COPYING file in the root directory of this source tree.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include "port/port.h"
#include "util/mutexlock.h"
#include "util/random.h"
#include "rocksdb/auto_tuner.h"
#include "rocksdb/env.h"
#include "rocksdb/rate_limiter.h"

namespace rocksdb {

class GenericRateLimiter : public RateLimiter {
 public:
  GenericRateLimiter(int64_t refill_bytes, int64_t refill_period_us,
                     int32_t fairness,
                     RateLimiter::Mode mode = RateLimiter::Mode::kWritesOnly);

  virtual ~GenericRateLimiter();

  // This API allows user to dynamically change rate limiter's bytes per second.
  virtual void SetBytesPerSecond(int64_t bytes_per_second) override;

  // Request for token to write bytes. If this request can not be satisfied,
  // the call is blocked. Caller is responsible to make sure
  // bytes <= GetSingleBurstBytes()
  using RateLimiter::Request;
  virtual void Request(const int64_t bytes, const Env::IOPriority pri,
                       Statistics* stats) override;

  virtual int64_t GetSingleBurstBytes() const override {
    return refill_bytes_per_period_.load(std::memory_order_relaxed);
  }

  virtual int64_t GetTotalBytesThrough(
      const Env::IOPriority pri = Env::IO_TOTAL) const override {
    MutexLock g(&request_mutex_);
    if (pri == Env::IO_TOTAL) {
      return total_bytes_through_[Env::IO_LOW] +
             total_bytes_through_[Env::IO_HIGH];
    }
    return total_bytes_through_[pri];
  }

  virtual int64_t GetTotalRequests(
      const Env::IOPriority pri = Env::IO_TOTAL) const override {
    MutexLock g(&request_mutex_);
    if (pri == Env::IO_TOTAL) {
      return total_requests_[Env::IO_LOW] + total_requests_[Env::IO_HIGH];
    }
    return total_requests_[pri];
  }

  virtual int64_t GetBytesPerSecond() const override {
    return rate_bytes_per_sec_;
  }

 private:
  void Refill();
  int64_t CalculateRefillBytesPerPeriod(int64_t rate_bytes_per_sec);
  uint64_t NowMicrosMonotonic(Env* env) {
    return env->NowNanos() / std::milli::den;
  }

  // This mutex guard all internal states
  mutable port::Mutex request_mutex_;

  const int64_t kMinRefillBytesPerPeriod = 100;

  const int64_t refill_period_us_;

  int64_t rate_bytes_per_sec_;
  // This variable can be changed dynamically.
  std::atomic<int64_t> refill_bytes_per_period_;
  Env* const env_;

  bool stop_;
  port::CondVar exit_cv_;
  int32_t requests_to_wait_;

  int64_t total_requests_[Env::IO_TOTAL];
  int64_t total_bytes_through_[Env::IO_TOTAL];
  int64_t available_bytes_;
  int64_t next_refill_us_;

  int32_t fairness_;
  Random rnd_;

  struct Req;
  Req* leader_;
  std::deque<Req*> queue_[Env::IO_TOTAL];
};

#ifndef ROCKSDB_LITE

// TODO(andrewkr): this is a temporary workaround to support auto-tuned rate
// limiter before we've written a scheduler/executor for auto-tuners. It does
// the scheduling/execution in a wrapper around the provided rate limiter.
class AdaptiveRateLimiter : public RateLimiter {
 public:
  AdaptiveRateLimiter(std::shared_ptr<RateLimiter> rate_limiter,
                      AutoTuner* tuner)
      : rate_limiter_(std::move(rate_limiter)), tuner_(tuner), last_tuned_(0) {}

  virtual void SetBytesPerSecond(int64_t bytes_per_second) override {
    rate_limiter_->SetBytesPerSecond(bytes_per_second);
  }
  using RateLimiter::Request;
  virtual void Request(const int64_t bytes, const Env::IOPriority pri,
                       Statistics* stats) override {
    std::chrono::milliseconds now(Env::Default()->NowMicros() / 1000);
    tuner_mutex_.lock();
    if (now - last_tuned_ >= tuner_->GetInterval()) {
      tuner_->Tune(now);
      last_tuned_ = now;
    }
    tuner_mutex_.unlock();
    rate_limiter_->Request(bytes, pri, stats);
  }
  virtual int64_t GetSingleBurstBytes() const override {
    return rate_limiter_->GetSingleBurstBytes();
  }
  virtual int64_t GetTotalBytesThrough(
      const Env::IOPriority pri = Env::IO_TOTAL) const override {
    return rate_limiter_->GetTotalBytesThrough(pri);
  }
  virtual int64_t GetTotalRequests(
      const Env::IOPriority pri = Env::IO_TOTAL) const override {
    return rate_limiter_->GetTotalRequests(pri);
  }
  virtual int64_t GetBytesPerSecond() const override {
    return rate_limiter_->GetBytesPerSecond();
  }

 private:
  // useful to make this a shared_ptr since typically it takes its value from
  // DBOptions::rate_limiter, which is a shared_ptr.
  std::shared_ptr<RateLimiter> rate_limiter_;
  AutoTuner* tuner_;
  std::mutex tuner_mutex_;
  std::chrono::milliseconds last_tuned_;
};

#endif  // ROCKSDB_LITE

}  // namespace rocksdb
