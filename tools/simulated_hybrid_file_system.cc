//  Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifndef ROCKSDB_LITE

#include "tools/simulated_hybrid_file_system.h"

#include <algorithm>
#include <sstream>
#include <string>

#include "rocksdb/rate_limiter.h"

namespace ROCKSDB_NAMESPACE {

const int kLatencyAddedPerRequestUs = 15000;
const int64_t kUsPerSec = 1000000;
const int64_t kDummyBytesPerUs = 1024;

namespace {
// From bytes to read/write, calculate service time needed by an HDD.
// This is used to simulate latency from HDD.
int CalculateServeTimeUs(size_t bytes) {
  return 12200 + static_cast<int>(static_cast<double>(bytes) * 0.005215);
}

// There is a bug in rater limiter that would crash with small requests
// Hack to get it around.
void RateLimiterRequest(RateLimiter* rater_limiter, int64_t amount) {
  int64_t left = amount * kDummyBytesPerUs;
  const int64_t kMaxToRequest = kDummyBytesPerUs * kUsPerSec / 1024;
  while (left > 0) {
    int64_t to_request = std::min(kMaxToRequest, left);
    rater_limiter->Request(to_request, Env::IOPriority::IO_LOW, nullptr);
    left -= to_request;
  }
}
}  // namespace

// The metadata file format: each line is a full filename of a file which is
// warm
SimulatedHybridFileSystem::SimulatedHybridFileSystem(
    const std::shared_ptr<FileSystem>& base,
    const std::string& metadata_file_name)
    : FileSystemWrapper(base),
      // Limit to 100 requests per second.
      rate_limiter_(NewGenericRateLimiter(
          kDummyBytesPerUs * kUsPerSec /* rate_bytes_per_sec */,
          1000 /* refill_period_us */)),
      metadata_file_name_(metadata_file_name),
      name_("SimulatedHybridFileSystem: " + std::string(target()->Name())) {
  IOStatus s = base->FileExists(metadata_file_name, IOOptions(), nullptr);
  if (s.IsNotFound()) {
    return;
  }
  std::string metadata;
  s = ReadFileToString(base.get(), metadata_file_name, &metadata);
  if (!s.ok()) {
    fprintf(stderr, "Error reading from file %s: %s",
            metadata_file_name.c_str(), s.ToString().c_str());
    // Exit rather than assert as this file system is built to run with
    // benchmarks, which usually run on release mode.
    std::exit(1);
  }
  std::istringstream input;
  input.str(metadata);
  std::string line;
  while (std::getline(input, line)) {
    fprintf(stderr, "Warm file %s\n", line.c_str());
    warm_file_set_.insert(line);
  }
}

// Need to write out the metadata file to file. See comment of
// SimulatedHybridFileSystem::SimulatedHybridFileSystem() for format of the
// file.
SimulatedHybridFileSystem::~SimulatedHybridFileSystem() {
  std::string metadata;
  for (const auto& f : warm_file_set_) {
    metadata += f;
    metadata += "\n";
  }
  IOStatus s = WriteStringToFile(target(), metadata, metadata_file_name_, true);
  if (!s.ok()) {
    fprintf(stderr, "Error writing to file %s: %s", metadata_file_name_.c_str(),
            s.ToString().c_str());
  }
}

IOStatus SimulatedHybridFileSystem::NewRandomAccessFile(
    const std::string& fname, const FileOptions& file_opts,
    std::unique_ptr<FSRandomAccessFile>* result, IODebugContext* dbg) {
  Temperature temperature = Temperature::kUnknown;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (warm_file_set_.find(fname) != warm_file_set_.end()) {
      temperature = Temperature::kWarm;
    }
  }
  assert(temperature == file_opts.temperature);
  IOStatus s = target()->NewRandomAccessFile(fname, file_opts, result, dbg);
  result->reset(
      new SimulatedHybridRaf(std::move(*result), rate_limiter_, temperature));
  return s;
}

IOStatus SimulatedHybridFileSystem::NewWritableFile(
    const std::string& fname, const FileOptions& file_opts,
    std::unique_ptr<FSWritableFile>* result, IODebugContext* dbg) {
  if (file_opts.temperature == Temperature::kWarm) {
    const std::lock_guard<std::mutex> lock(mutex_);
    warm_file_set_.insert(fname);
  }

  IOStatus s = target()->NewWritableFile(fname, file_opts, result, dbg);
  if (file_opts.temperature == Temperature::kWarm) {
    result->reset(new SimulatedWritableFile(std::move(*result), rate_limiter_));
  }
  return s;
}

IOStatus SimulatedHybridFileSystem::DeleteFile(const std::string& fname,
                                               const IOOptions& options,
                                               IODebugContext* dbg) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    warm_file_set_.erase(fname);
  }
  return target()->DeleteFile(fname, options, dbg);
}

IOStatus SimulatedHybridRaf::Read(uint64_t offset, size_t n,
                                  const IOOptions& options, Slice* result,
                                  char* scratch, IODebugContext* dbg) const {
  if (temperature_ == Temperature::kWarm) {
    Env::Default()->SleepForMicroseconds(kLatencyAddedPerRequestUs);
    RequestRateLimit(n);
  }
  return target()->Read(offset, n, options, result, scratch, dbg);
}

IOStatus SimulatedHybridRaf::MultiRead(FSReadRequest* reqs, size_t num_reqs,
                                       const IOOptions& options,
                                       IODebugContext* dbg) {
  if (temperature_ == Temperature::kWarm) {
    for (size_t i = 0; i < num_reqs; i++) {
      RequestRateLimit(reqs[i].len);
    }
    Env::Default()->SleepForMicroseconds(kLatencyAddedPerRequestUs *
                                         static_cast<int>(num_reqs));
  }
  return target()->MultiRead(reqs, num_reqs, options, dbg);
}

IOStatus SimulatedHybridRaf::Prefetch(uint64_t offset, size_t n,
                                      const IOOptions& options,
                                      IODebugContext* dbg) {
  if (temperature_ == Temperature::kWarm) {
    RequestRateLimit(n);
    Env::Default()->SleepForMicroseconds(kLatencyAddedPerRequestUs);
  }
  return target()->Prefetch(offset, n, options, dbg);
}

void SimulatedHybridRaf::RequestRateLimit(int64_t bytes) const {
  RateLimiterRequest(rate_limiter_.get(), CalculateServeTimeUs(bytes));
}

void SimulatedWritableFile::RequestRateLimit(int64_t bytes) const {
  RateLimiterRequest(rate_limiter_.get(), CalculateServeTimeUs(bytes));
}

IOStatus SimulatedWritableFile::Append(const Slice& data, const IOOptions& ioo,
                                       IODebugContext* idc) {
  if (use_direct_io()) {
    RequestRateLimit(data.size());
  } else {
    unsynced_bytes += data.size();
  }
  return target()->Append(data, ioo, idc);
}

IOStatus SimulatedWritableFile::Append(
    const Slice& data, const IOOptions& options,
    const DataVerificationInfo& verification_info, IODebugContext* dbg) {
  if (use_direct_io()) {
    RequestRateLimit(data.size());
  } else {
    unsynced_bytes += data.size();
  }
  return target()->Append(data, options, verification_info, dbg);
}

IOStatus SimulatedWritableFile::PositionedAppend(
    const Slice& data, uint64_t offset, const IOOptions& options,
    const DataVerificationInfo& verification_info, IODebugContext* dbg) {
  if (use_direct_io()) {
    RequestRateLimit(data.size());
  } else {
    // This might be overcalculated, but it's probably OK.
    unsynced_bytes += data.size();
  }
  return target()->PositionedAppend(data, offset, options, verification_info,
                                    dbg);
}

IOStatus SimulatedWritableFile::Sync(const IOOptions& options,
                                     IODebugContext* dbg) {
  if (unsynced_bytes > 0) {
    RequestRateLimit(unsynced_bytes);
    unsynced_bytes = 0;
  }
  return target()->Sync(options, dbg);
}
}  // namespace ROCKSDB_NAMESPACE

#endif  // ROCKSDB_LITE
