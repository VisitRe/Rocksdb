//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "file/random_access_file_reader.h"

#include <algorithm>
#include <mutex>

#include "file/file_util.h"
#include "monitoring/histogram.h"
#include "monitoring/iostats_context_imp.h"
#include "port/port.h"
#include "table/format.h"
#include "test_util/sync_point.h"
#include "util/random.h"
#include "util/rate_limiter.h"

namespace ROCKSDB_NAMESPACE {
Status RandomAccessFileReader::Create(
    const std::shared_ptr<FileSystem>& fs, const std::string& fname,
    const FileOptions& file_opts,
    std::unique_ptr<RandomAccessFileReader>* reader, IODebugContext* dbg) {
  std::unique_ptr<FSRandomAccessFile> file;
  Status s = fs->NewRandomAccessFile(fname, file_opts, &file, dbg);
  if (s.ok()) {
    reader->reset(new RandomAccessFileReader(std::move(file), fname));
  }
  return s;
}

ThreadLocalPtr RandomAccessFileReader::context_pool_ptr;

Status RandomAccessFileReader::Read(const IOOptions& opts, uint64_t offset,
                                    size_t n, Slice* result, char* scratch,
                                    AlignedBuf* aligned_buf,
                                    bool for_compaction) const {
  (void)aligned_buf;

  TEST_SYNC_POINT_CALLBACK("RandomAccessFileReader::Read", nullptr);
  Status s;
  uint64_t elapsed = 0;
  {
    StopWatch sw(clock_, stats_, hist_type_,
                 (stats_ != nullptr) ? &elapsed : nullptr, true /*overwrite*/,
                 true /*delay_enabled*/);
    auto prev_perf_level = GetPerfLevel();
    IOSTATS_TIMER_GUARD(read_nanos);
    if (use_direct_io()) {
#ifndef ROCKSDB_LITE
      size_t alignment = file_->GetRequiredBufferAlignment();
      size_t aligned_offset =
          TruncateToPageBoundary(alignment, static_cast<size_t>(offset));
      size_t offset_advance = static_cast<size_t>(offset) - aligned_offset;
      size_t read_size =
          Roundup(static_cast<size_t>(offset + n), alignment) - aligned_offset;
      AlignedBuffer buf;
      buf.Alignment(alignment);
      buf.AllocateNewBuffer(read_size);
      while (buf.CurrentSize() < read_size) {
        size_t allowed;
        if (for_compaction && rate_limiter_ != nullptr) {
          allowed = rate_limiter_->RequestToken(
              buf.Capacity() - buf.CurrentSize(), buf.Alignment(),
              Env::IOPriority::IO_LOW, stats_, RateLimiter::OpType::kRead);
        } else {
          assert(buf.CurrentSize() == 0);
          allowed = read_size;
        }
        Slice tmp;

        FileOperationInfo::StartTimePoint start_ts;
        uint64_t orig_offset = 0;
        if (ShouldNotifyListeners()) {
          start_ts = FileOperationInfo::StartNow();
          orig_offset = aligned_offset + buf.CurrentSize();
        }

        {
          IOSTATS_CPU_TIMER_GUARD(cpu_read_nanos, clock_);
          // Only user reads are expected to specify a timeout. And user reads
          // are not subjected to rate_limiter and should go through only
          // one iteration of this loop, so we don't need to check and adjust
          // the opts.timeout before calling file_->Read
          assert(!opts.timeout.count() || allowed == read_size);
          s = file_->Read(aligned_offset + buf.CurrentSize(), allowed, opts,
                          &tmp, buf.Destination(), nullptr);
        }
        if (ShouldNotifyListeners()) {
          auto finish_ts = FileOperationInfo::FinishNow();
          NotifyOnFileReadFinish(orig_offset, tmp.size(), start_ts, finish_ts,
                                 s);
        }

        buf.Size(buf.CurrentSize() + tmp.size());
        if (!s.ok() || tmp.size() < allowed) {
          break;
        }
      }
      size_t res_len = 0;
      if (s.ok() && offset_advance < buf.CurrentSize()) {
        res_len = std::min(buf.CurrentSize() - offset_advance, n);
        if (aligned_buf == nullptr) {
          buf.Read(scratch, offset_advance, res_len);
        } else {
          scratch = buf.BufferStart() + offset_advance;
          aligned_buf->reset(buf.Release());
        }
      }
      *result = Slice(scratch, res_len);
#endif  // !ROCKSDB_LITE
    } else {
      size_t pos = 0;
      const char* res_scratch = nullptr;
      while (pos < n) {
        size_t allowed;
        if (for_compaction && rate_limiter_ != nullptr) {
          if (rate_limiter_->IsRateLimited(RateLimiter::OpType::kRead)) {
            sw.DelayStart();
          }
          allowed = rate_limiter_->RequestToken(n - pos, 0 /* alignment */,
                                                Env::IOPriority::IO_LOW, stats_,
                                                RateLimiter::OpType::kRead);
          if (rate_limiter_->IsRateLimited(RateLimiter::OpType::kRead)) {
            sw.DelayStop();
          }
        } else {
          allowed = n;
        }
        Slice tmp_result;

#ifndef ROCKSDB_LITE
        FileOperationInfo::StartTimePoint start_ts;
        if (ShouldNotifyListeners()) {
          start_ts = FileOperationInfo::StartNow();
        }
#endif

        {
          IOSTATS_CPU_TIMER_GUARD(cpu_read_nanos, clock_);
          // Only user reads are expected to specify a timeout. And user reads
          // are not subjected to rate_limiter and should go through only
          // one iteration of this loop, so we don't need to check and adjust
          // the opts.timeout before calling file_->Read
          assert(!opts.timeout.count() || allowed == n);
          s = file_->Read(offset + pos, allowed, opts, &tmp_result,
                          scratch + pos, nullptr);
        }
#ifndef ROCKSDB_LITE
        if (ShouldNotifyListeners()) {
          auto finish_ts = FileOperationInfo::FinishNow();
          NotifyOnFileReadFinish(offset + pos, tmp_result.size(), start_ts,
                                 finish_ts, s);
        }
#endif

        if (res_scratch == nullptr) {
          // we can't simply use `scratch` because reads of mmap'd files return
          // data in a different buffer.
          res_scratch = tmp_result.data();
        } else {
          // make sure chunks are inserted contiguously into `res_scratch`.
          assert(tmp_result.data() == res_scratch + pos);
        }
        pos += tmp_result.size();
        if (!s.ok() || tmp_result.size() < allowed) {
          break;
        }
      }
      *result = Slice(res_scratch, s.ok() ? pos : 0);
    }
    IOSTATS_ADD_IF_POSITIVE(bytes_read, result->size());
    SetPerfLevel(prev_perf_level);
  }
  if (stats_ != nullptr && file_read_hist_ != nullptr) {
    file_read_hist_->Add(elapsed);
  }

  return s;
}

size_t End(const FSReadRequest& r) {
  return static_cast<size_t>(r.offset) + r.len;
}

FSReadRequest Align(const FSReadRequest& r, size_t alignment) {
  FSReadRequest req;
  req.offset = static_cast<uint64_t>(
    TruncateToPageBoundary(alignment, static_cast<size_t>(r.offset)));
  req.len = Roundup(End(r), alignment) - req.offset;
  req.scratch = nullptr;
  return req;
}

bool TryMerge(FSReadRequest* dest, const FSReadRequest& src) {
  size_t dest_offset = static_cast<size_t>(dest->offset);
  size_t src_offset = static_cast<size_t>(src.offset);
  size_t dest_end = End(*dest);
  size_t src_end = End(src);
  if (std::max(dest_offset, src_offset) > std::min(dest_end, src_end)) {
    return false;
  }
  dest->offset = static_cast<uint64_t>(std::min(dest_offset, src_offset));
  dest->len = std::max(dest_end, src_end) - dest->offset;
  return true;
}

Status RandomAccessFileReader::MultiRead(const IOOptions& opts,
                                         FSReadRequest* read_reqs,
                                         size_t num_reqs,
                                         AlignedBuf* aligned_buf) const {
  (void)aligned_buf;  // suppress warning of unused variable in LITE mode
  assert(num_reqs > 0);
  Status s;
  uint64_t elapsed = 0;
  {
    StopWatch sw(clock_, stats_, hist_type_,
                 (stats_ != nullptr) ? &elapsed : nullptr, true /*overwrite*/,
                 true /*delay_enabled*/);
    auto prev_perf_level = GetPerfLevel();
    IOSTATS_TIMER_GUARD(read_nanos);

    FSReadRequest* fs_reqs = read_reqs;
    size_t num_fs_reqs = num_reqs;
#ifndef ROCKSDB_LITE
    std::vector<FSReadRequest> aligned_reqs;
    if (use_direct_io()) {
      // num_reqs is the max possible size,
      // this can reduce std::vecector's internal resize operations.
      aligned_reqs.reserve(num_reqs);
      // Align and merge the read requests.
      size_t alignment = file_->GetRequiredBufferAlignment();
      for (size_t i = 0; i < num_reqs; i++) {
        const auto& r = Align(read_reqs[i], alignment);
        if (i == 0) {
          // head
          aligned_reqs.push_back(r);

        } else if (!TryMerge(&aligned_reqs.back(), r)) {
          // head + n
          aligned_reqs.push_back(r);

        } else {
          // unused
          r.status.PermitUncheckedError();
        }
      }
      TEST_SYNC_POINT_CALLBACK("RandomAccessFileReader::MultiRead:AlignedReqs",
                               &aligned_reqs);

      // Allocate aligned buffer and let scratch buffers point to it.
      size_t total_len = 0;
      for (const auto& r : aligned_reqs) {
        total_len += r.len;
      }
      AlignedBuffer buf;
      buf.Alignment(alignment);
      buf.AllocateNewBuffer(total_len);
      char* scratch = buf.BufferStart();
      for (auto& r : aligned_reqs) {
        r.scratch = scratch;
        scratch += r.len;
      }

      aligned_buf->reset(buf.Release());
      fs_reqs = aligned_reqs.data();
      num_fs_reqs = aligned_reqs.size();
    }
#endif  // ROCKSDB_LITE

#ifndef ROCKSDB_LITE
    FileOperationInfo::StartTimePoint start_ts;
    if (ShouldNotifyListeners()) {
      start_ts = FileOperationInfo::StartNow();
    }
#endif  // ROCKSDB_LITE

    {
      IOSTATS_CPU_TIMER_GUARD(cpu_read_nanos, clock_);
      s = file_->MultiRead(fs_reqs, num_fs_reqs, opts, nullptr);
    }

#ifndef ROCKSDB_LITE
    if (use_direct_io()) {
      // Populate results in the unaligned read requests.
      size_t aligned_i = 0;
      for (size_t i = 0; i < num_reqs; i++) {
        auto& r = read_reqs[i];
        if (static_cast<size_t>(r.offset) > End(aligned_reqs[aligned_i])) {
          aligned_i++;
        }
        const auto& fs_r = fs_reqs[aligned_i];
        r.status = fs_r.status;
        if (r.status.ok()) {
          uint64_t offset = r.offset - fs_r.offset;
          size_t len = std::min(r.len, static_cast<size_t>(fs_r.len - offset));
          r.result = Slice(fs_r.scratch + offset, len);
        } else {
          r.result = Slice();
        }
      }
    }
#endif  // ROCKSDB_LITE

    for (size_t i = 0; i < num_reqs; ++i) {
#ifndef ROCKSDB_LITE
      if (ShouldNotifyListeners()) {
        auto finish_ts = FileOperationInfo::FinishNow();
        NotifyOnFileReadFinish(read_reqs[i].offset, read_reqs[i].result.size(),
                               start_ts, finish_ts, read_reqs[i].status);
      }
#endif  // ROCKSDB_LITE
      IOSTATS_ADD_IF_POSITIVE(bytes_read, read_reqs[i].result.size());
    }
    SetPerfLevel(prev_perf_level);
  }
  if (stats_ != nullptr && file_read_hist_ != nullptr) {
    file_read_hist_->Add(elapsed);
  }

  return s;
}

IOStatus RandomAccessFileReader::PrepareIOOptions(const ReadOptions& ro,
                                                  IOOptions& opts) {
  if (clock_.get() != nullptr) {
    return PrepareIOFromReadOptions(ro, clock_, opts);
  } else {
    return PrepareIOFromReadOptions(ro, SystemClock::Default(), opts);
  }
}

Status RandomAccessFileReader::MultiReadAsync(
    const IOOptions& opts, IOCallback cb, void* cb_arg1, void* cb_arg2,
    FSReadRequest* read_reqs, size_t num_reqs,
    std::unique_ptr<void, IOHandleDeleter>* handle,
    AlignedBuf* aligned_buf) const {
  (void)aligned_buf;  // suppress warning of unused variable in LITE mode
  assert(num_reqs > 0);
  Status s;
  uint64_t elapsed = 0;

  {
    StopWatch sw(clock_, stats_, hist_type_,
                 (stats_ != nullptr) ? &elapsed : nullptr, true /*overwrite*/,
                 true /*delay_enabled*/);
    MultiReadContextPool* ctx_pool =
        static_cast<MultiReadContextPool*>(context_pool_ptr.Get());
    if (!ctx_pool) {
      ctx_pool = new MultiReadContextPool;
      context_pool_ptr.Reset(ctx_pool);
    }

    MultiReadContext* ctx = ctx_pool->Allocate(num_reqs);
    auto prev_perf_level = GetPerfLevel();
    FSReadRequest* fs_reqs = read_reqs;
    size_t num_fs_reqs = num_reqs;
#ifndef ROCKSDB_LITE
    std::vector<FSReadRequest> aligned_reqs;
    ctx->cb = cb;
    ctx->cb_arg1 = cb_arg1;
    ctx->cb_arg2 = cb_arg2;

    if (use_direct_io()) {
      // num_reqs is the max possible size,
      // this can reduce std::vecector's internal resize operations.
      aligned_reqs.reserve(num_reqs);
      // Align and merge the read requests.
      size_t alignment = file_->GetRequiredBufferAlignment();
      aligned_reqs.push_back(Align(read_reqs[0], alignment));
      ctx->reqs.emplace_back(read_reqs[0]);
      for (size_t i = 1; i < num_reqs; i++) {
        const auto& r = Align(read_reqs[i], alignment);
        if (!TryMerge(&aligned_reqs.back(), r)) {
          aligned_reqs.push_back(r);
        }
        ctx->reqs.emplace_back(read_reqs[i]);
      }
      TEST_SYNC_POINT_CALLBACK("RandomAccessFileReader::MultiRead:AlignedReqs",
                               &aligned_reqs);

      // Allocate aligned buffer and let scratch buffers point to it.
      size_t total_len = 0;
      for (const auto& r : aligned_reqs) {
        total_len += r.len;
      }
      AlignedBuffer buf;
      buf.Alignment(alignment);
      buf.AllocateNewBuffer(total_len);
      char* scratch = buf.BufferStart();
      for (auto& r : aligned_reqs) {
        r.scratch = scratch;
        scratch += r.len;
      }

      aligned_buf->reset(buf.Release());
      fs_reqs = aligned_reqs.data();
      num_fs_reqs = aligned_reqs.size();
    } else {
      for (size_t i = 0; i < num_reqs; ++i) {
        ctx->reqs.emplace_back(read_reqs[i]);
      }
    }
#endif  // ROCKSDB_LITE

#ifndef ROCKSDB_LITE
    FileOperationInfo::StartTimePoint start_ts;
    if (ShouldNotifyListeners()) {
      start_ts = FileOperationInfo::StartNow();
    }
    ctx->start_ts = start_ts;
#endif  // ROCKSDB_LITE

    {
      IOSTATS_CPU_TIMER_GUARD(cpu_read_nanos, clock_);
      s = file_->MultiReadAsync(
          opts, &RandomAccessFileReader::MultiReadCallback, this, ctx,
          num_fs_reqs, fs_reqs, &ctx->handle, nullptr);
    }
    SetPerfLevel(prev_perf_level);

    std::unique_ptr<void, IOHandleDeleter> hndl(
        ctx, [](void* p) { delete static_cast<MultiReadContext*>(p); });
    *handle = std::move(hndl);
  }
  if (stats_ != nullptr && file_read_hist_ != nullptr) {
    file_read_hist_->Add(elapsed);
  }

  return s;
}

void RandomAccessFileReader::MultiReadAsyncStage2(const FSReadResponse* resps,
                                                  const size_t num_resps,
                                                  MultiReadContext* ctx) {
  {
#ifndef ROCKSDB_LITE
    if (use_direct_io()) {
      // Populate results in the unaligned read requests.
      size_t aligned_i = 0;
      size_t alignment = file_->GetRequiredBufferAlignment();
      size_t i = 0;
      size_t buf_offset = ULONG_MAX;
      size_t prev_offset = 0;

      while (i < ctx->reqs.size()) {
        FSReadRequest& req = ctx->reqs[i];
        FSReadResponse& resp = ctx->resps[i];
        const FSReadResponse& file_resp = resps[aligned_i];
        if (buf_offset == ULONG_MAX) {
          // First request overlapping this response
          size_t aligned_offset = static_cast<uint64_t>(TruncateToPageBoundary(
              alignment, static_cast<size_t>(req.offset)));
          buf_offset = req.offset - aligned_offset;
        } else {
          buf_offset += req.offset - prev_offset;
        }
        prev_offset = req.offset;
        if (req.len + buf_offset > file_resp.result.size()) {
          aligned_i++;
          buf_offset = ULONG_MAX;
          continue;
        }
        resp.status = file_resp.status;
        if (resp.status.ok()) {
          size_t len = std::min(
              req.len,
              static_cast<size_t>(file_resp.result.size() - buf_offset));
          resp.result = Slice(file_resp.result.data() + buf_offset, len);
        } else {
          resp.result = Slice();
        }
        i++;
      }
    }
#endif  // ROCKSDB_LITE

    for (size_t i = 0; i < ctx->reqs.size(); ++i) {
#ifndef ROCKSDB_LITE
      if (ShouldNotifyListeners()) {
        auto finish_ts = FileOperationInfo::FinishNow();
        NotifyOnFileReadFinish(ctx->reqs[i].offset, ctx->resps[i].result.size(),
                               ctx->start_ts, finish_ts, ctx->resps[i].status);
      }
#endif  // ROCKSDB_LITE
      IOSTATS_ADD_IF_POSITIVE(bytes_read, ctx->resps[i].result.size());
    }
  }

  IOCallback cb = ctx->cb;
  (*cb)(use_direct_io() ? ctx->resps.data() : resps,
        use_direct_io() ? ctx->resps.size() : num_resps, ctx->cb_arg1,
        ctx->cb_arg2);
}

void RandomAccessFileReader::MultiReadCallback(const FSReadResponse* resps,
                                               const size_t num_resps,
                                               void* cb_arg1, void* cb_arg2) {
  RandomAccessFileReader* reader =
      static_cast<RandomAccessFileReader*>(cb_arg1);
  MultiReadContext* ctx = static_cast<MultiReadContext*>(cb_arg2);

  reader->MultiReadAsyncStage2(resps, num_resps, ctx);
}
}  // namespace ROCKSDB_NAMESPACE
