//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <functional>
#include <string>

#include "port/port.h"
#include "rocksdb/env.h"
#include "util/mutexlock.h"

namespace rocksdb {

class RepeatableThread {
 public:
  RepeatableThread(std::function<void()> function,
                   const std::string& thread_name, Env* env, uint64_t delay_us,
                   uint64_t initial_delay_us = 0)
      : function_(function),
        thread_name_("rocksdb:" + thread_name),
        env_(env),
        delay_us_(delay_us),
        initial_delay_us_(initial_delay_us),
        cond_var_(&mutex_),
        running_(true),
#ifndef NDEBUG
        waiting_(false),
        executed_(false),
#endif
        thread_([this] { thread(); }) {
  }

  void cancel() {
    {
      MutexLock l(&mutex_);
      if (!running_) {
        return;
      }
      running_ = false;
      cond_var_.SignalAll();
    }
    thread_.join();
  }

  ~RepeatableThread() { cancel(); }

#ifndef NDEBUG
  // Wait until RepeatableThread starting waiting, call the optional callback,
  // then wait for one run of RepeatableThread. Tests can use provide a
  // custom env object to mock time, and use the callback here to bump current
  // time and trigger RepeatableThread. See repeatable_thread_test for example.
  //
  // Note: only support one caller of this method.
  void TEST_WaitForRun(std::function<void()> callback = nullptr) {
    MutexLock l(&mutex_);
    while (!waiting_) {
      cond_var_.Wait();
    }
    if (callback != nullptr) {
      callback();
    }
    executed_ = false;
    cond_var_.SignalAll();
    while (!executed_) {
      cond_var_.Wait();
    }
  }
#endif

 private:
  bool wait(uint64_t delay) {
    MutexLock l(&mutex_);
    if (running_ && delay > 0) {
      uint64_t wait_until = env_->NowMicros() + delay;
#ifndef NDEBUG
      waiting_ = true;
      cond_var_.SignalAll();
#endif
      while (running_) {
        cond_var_.TimedWait(wait_until);
        if (env_->NowMicros() >= wait_until) {
          break;
        }
      }
#ifndef NDEBUG
      waiting_ = false;
#endif
    }
    return running_;
  }

  void thread() {
#if defined(_GNU_SOURCE) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 12)
    // Set thread name.
    auto thread_handle = thread_.native_handle();
    int ret __attribute__((__unused__)) =
        pthread_setname_np(thread_handle, thread_name_.c_str());
    assert(ret == 0);
#endif
#endif

    assert(delay_us_ > 0);
    if (!wait(initial_delay_us_)) {
      return;
    }
    do {
      function_();
#ifndef NDEBUG
      executed_ = true;
      cond_var_.SignalAll();
#endif
    } while (wait(delay_us_));
  }

  const std::function<void()> function_;
  const std::string thread_name_;
  Env* const env_;
  const uint64_t delay_us_;
  const uint64_t initial_delay_us_;

  port::Mutex mutex_;
  port::CondVar cond_var_;
  bool running_;
#ifndef NDEBUG
  // RepeatableThread waiting for timeout.
  bool waiting_;
  // Function ran after caller of TEST_WaitForRun start waiting.
  bool executed_;
#endif
  port::Thread thread_;
};

}  // namespace rocksdb
