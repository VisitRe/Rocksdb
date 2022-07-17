//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "db/periodic_work_scheduler.h"

#include "rocksdb/system_clock.h"

#ifndef ROCKSDB_LITE
namespace ROCKSDB_NAMESPACE {

constexpr uint64_t kMicrosInSecond = 1000 * 1000;

// `timer_mu_` serves two purposes currently:
// (1) to ensure calls to `Start()` and `Shutdown()` are serialized, as
//     they are currently not implemented in a thread-safe way; and
// (2) to ensure the `Timer::Add()`s and `Timer::Start()` run atomically, and
//     the `Timer::Cancel()`s and `Timer::Shutdown()` run atomically.
static port::Mutex timer_mu_;

static const std::map<PeriodicTaskType, uint64_t> kDefaultPeriodSeconds = {
    {PeriodicTaskType::kDumpStats, kInvalidPeriodSec},
    {PeriodicTaskType::kPersistStats, kInvalidPeriodSec},
    {PeriodicTaskType::kFlushInfoLog, 10},
    {PeriodicTaskType::kRecordSeqnoTime, kInvalidPeriodSec},
};

Status PeriodicTaskScheduler::Register(PeriodicTaskType task_type,
                                       const PeriodicTaskFunc& fn) {
  return Register(task_type, std::move(fn),
                  kDefaultPeriodSeconds.at(task_type));
}

Status PeriodicTaskScheduler::Register(PeriodicTaskType task_type,
                                       const PeriodicTaskFunc& fn,
                                       uint64_t repeat_period_seconds) {
  MutexLock l(&timer_mu_);
  static std::atomic<uint64_t> initial_delay(0);

  if (repeat_period_seconds == kInvalidPeriodSec) {
    return Status::InvalidArgument("Invalid task repeat period");
  }
  auto it = tasks_map_.find(task_type);
  if (it != tasks_map_.end()) {
    // the task already exists and it's the same, no update needed
    if (it->second.repeat_every_sec == repeat_period_seconds) {
      return Status::OK();
    }
    // cancel the existing one before register new one
    timer_->Cancel(it->second.name);
    tasks_map_.erase(it);
  }

  timer_->Start();
  std::string unique_id = env_.GenerateUniqueId();

  bool succeeded = timer_->Add(
      fn, unique_id,
      (initial_delay.fetch_add(1) % repeat_period_seconds) * kMicrosInSecond,
      repeat_period_seconds * kMicrosInSecond);
  if (!succeeded) {
    return Status::Aborted("Failed to register periodic task");
  }
  auto result = tasks_map_.try_emplace(
      task_type, TaskInfo{unique_id, repeat_period_seconds});
  assert(result.second);
  return Status::OK();
}

Status PeriodicTaskScheduler::Unregister(PeriodicTaskType task_type) {
  MutexLock l(&timer_mu_);
  auto it = tasks_map_.find(task_type);
  if (it != tasks_map_.end()) {
    timer_->Cancel(it->second.name);
    tasks_map_.erase(it);
  }
  if (!timer_->HasPendingTask()) {
    timer_->Shutdown();
  }
  return Status::OK();
}

void PeriodicTaskScheduler::TEST_OverrideTimer(SystemClock* clock) {
  static Timer test_timer(clock);
  test_timer.TEST_OverrideTimer(clock);
  MutexLock l(&timer_mu_);
  timer_ = &test_timer;
}

}  // namespace ROCKSDB_NAMESPACE

#endif  // ROCKSDB_LITE
