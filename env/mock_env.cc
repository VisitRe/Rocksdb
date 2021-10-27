//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "env/mock_env.h"

#include <algorithm>
#include <chrono>

#include "file/filename.h"
#include "port/sys_time.h"
#include "rocksdb/file_system.h"
#include "test_util/sync_point.h"
#include "util/cast_util.h"
#include "util/hash.h"
#include "util/random.h"
#include "util/rate_limiter.h"

namespace ROCKSDB_NAMESPACE {

class MemFile {
 public:
  explicit MemFile(Env* env, const std::string& fn, bool _is_lock_file = false)
      : env_(env),
        fn_(fn),
        refs_(0),
        is_lock_file_(_is_lock_file),
        locked_(false),
        size_(0),
        modified_time_(Now()),
        rnd_(Lower32of64(GetSliceNPHash64(fn))),
        fsynced_bytes_(0) {}
  // No copying allowed.
  MemFile(const MemFile&) = delete;
  void operator=(const MemFile&) = delete;

  void Ref() {
    MutexLock lock(&mutex_);
    ++refs_;
  }

  bool is_lock_file() const { return is_lock_file_; }

  bool Lock() {
    assert(is_lock_file_);
    MutexLock lock(&mutex_);
    if (locked_) {
      return false;
    } else {
      locked_ = true;
      return true;
    }
  }

  void Unlock() {
    assert(is_lock_file_);
    MutexLock lock(&mutex_);
    locked_ = false;
  }

  void Unref() {
    bool do_delete = false;
    {
      MutexLock lock(&mutex_);
      --refs_;
      assert(refs_ >= 0);
      if (refs_ <= 0) {
        do_delete = true;
      }
    }

    if (do_delete) {
      delete this;
    }
  }

  uint64_t Size() const { return size_; }

  void Truncate(size_t size, const IOOptions& /*options*/,
                IODebugContext* /*dbg*/) {
    MutexLock lock(&mutex_);
    if (size < size_) {
      data_.resize(size);
      size_ = size;
    }
  }

  void CorruptBuffer() {
    if (fsynced_bytes_ >= size_) {
      return;
    }
    uint64_t buffered_bytes = size_ - fsynced_bytes_;
    uint64_t start =
        fsynced_bytes_ + rnd_.Uniform(static_cast<int>(buffered_bytes));
    uint64_t end = std::min(start + 512, size_.load());
    MutexLock lock(&mutex_);
    for (uint64_t pos = start; pos < end; ++pos) {
      data_[static_cast<size_t>(pos)] = static_cast<char>(rnd_.Uniform(256));
    }
  }

  IOStatus Read(uint64_t offset, size_t n, const IOOptions& /*options*/,
                Slice* result, char* scratch, IODebugContext* /*dbg*/) const {
    {
      IOStatus s;
      TEST_SYNC_POINT_CALLBACK("MemFile::Read:IOStatus", &s);
      if (!s.ok()) {
        // with sync point only
        *result = Slice();
        return s;
      }
    }
    MutexLock lock(&mutex_);
    const uint64_t available = Size() - std::min(Size(), offset);
    size_t offset_ = static_cast<size_t>(offset);
    if (n > available) {
      n = static_cast<size_t>(available);
    }
    if (n == 0) {
      *result = Slice();
      return IOStatus::OK();
    }
    if (scratch) {
      memcpy(scratch, &(data_[offset_]), n);
      *result = Slice(scratch, n);
    } else {
      *result = Slice(&(data_[offset_]), n);
    }
    return IOStatus::OK();
  }

  IOStatus Write(uint64_t offset, const Slice& data,
                 const IOOptions& /*options*/, IODebugContext* /*dbg*/) {
    MutexLock lock(&mutex_);
    size_t offset_ = static_cast<size_t>(offset);
    if (offset + data.size() > data_.size()) {
      data_.resize(offset_ + data.size());
    }
    data_.replace(offset_, data.size(), data.data(), data.size());
    size_ = data_.size();
    modified_time_ = Now();
    return IOStatus::OK();
  }

  IOStatus Append(const Slice& data, const IOOptions& /*options*/,
                  IODebugContext* /*dbg*/) {
    MutexLock lock(&mutex_);
    data_.append(data.data(), data.size());
    size_ = data_.size();
    modified_time_ = Now();
    return IOStatus::OK();
  }

  IOStatus Fsync(const IOOptions& /*options*/, IODebugContext* /*dbg*/) {
    fsynced_bytes_ = size_.load();
    return IOStatus::OK();
  }

  uint64_t ModifiedTime() const { return modified_time_; }

 private:
  uint64_t Now() {
    int64_t unix_time = 0;
    auto s = env_->GetCurrentTime(&unix_time);
    assert(s.ok());
    return static_cast<uint64_t>(unix_time);
  }

  // Private since only Unref() should be used to delete it.
  ~MemFile() { assert(refs_ == 0); }

  Env* env_;
  const std::string fn_;
  mutable port::Mutex mutex_;
  int refs_;
  bool is_lock_file_;
  bool locked_;

  // Data written into this file, all bytes before fsynced_bytes are
  // persistent.
  std::string data_;
  std::atomic<uint64_t> size_;
  std::atomic<uint64_t> modified_time_;

  Random rnd_;
  std::atomic<uint64_t> fsynced_bytes_;
};

namespace {

class MockSequentialFile : public FSSequentialFile {
 public:
  explicit MockSequentialFile(MemFile* file, const FileOptions& opts)
      : file_(file),
        use_direct_io_(opts.use_direct_reads),
        use_mmap_read_(opts.use_mmap_reads),
        pos_(0) {
    file_->Ref();
  }

  ~MockSequentialFile() override { file_->Unref(); }

  IOStatus Read(size_t n, const IOOptions& options, Slice* result,
                char* scratch, IODebugContext* dbg) override {
    IOStatus s = file_->Read(pos_, n, options, result,
                             (use_mmap_read_) ? nullptr : scratch, dbg);
    if (s.ok()) {
      pos_ += result->size();
    }
    return s;
  }

  bool use_direct_io() const override { return use_direct_io_; }
  IOStatus Skip(uint64_t n) override {
    if (pos_ > file_->Size()) {
      return IOStatus::IOError("pos_ > file_->Size()");
    }
    const uint64_t available = file_->Size() - pos_;
    if (n > available) {
      n = available;
    }
    pos_ += static_cast<size_t>(n);
    return IOStatus::OK();
  }

 private:
  MemFile* file_;
  bool use_direct_io_;
  bool use_mmap_read_;
  size_t pos_;
};

class MockRandomAccessFile : public FSRandomAccessFile {
 public:
  explicit MockRandomAccessFile(MemFile* file, const FileOptions& opts)
      : file_(file),
        use_direct_io_(opts.use_direct_reads),
        use_mmap_read_(opts.use_mmap_reads) {
    file_->Ref();
  }

  ~MockRandomAccessFile() override { file_->Unref(); }

  bool use_direct_io() const override { return use_direct_io_; }

  IOStatus Prefetch(uint64_t /*offset*/, size_t /*n*/,
                    const IOOptions& /*options*/,
                    IODebugContext* /*dbg*/) override {
    return IOStatus::OK();
  }

  IOStatus Read(uint64_t offset, size_t n, const IOOptions& options,
                Slice* result, char* scratch,
                IODebugContext* dbg) const override {
    if (use_mmap_read_) {
      return file_->Read(offset, n, options, result, nullptr, dbg);
    } else {
      return file_->Read(offset, n, options, result, scratch, dbg);
    }
  }

    async_result AsyncRead(uint64_t offset, size_t n, const IOOptions& options,
                Slice* result, char* scratch,
                IODebugContext* dbg) const override {
    (void)offset;
    (void)n;
    (void)options;
    (void)result;
    (void)scratch;
    (void)dbg;
    throw "Not implemented";
  }

 private:
  MemFile* file_;
  bool use_direct_io_;
  bool use_mmap_read_;
};

class MockRandomRWFile : public FSRandomRWFile {
 public:
  explicit MockRandomRWFile(MemFile* file) : file_(file) { file_->Ref(); }

  ~MockRandomRWFile() override { file_->Unref(); }

  IOStatus Write(uint64_t offset, const Slice& data, const IOOptions& options,
                 IODebugContext* dbg) override {
    return file_->Write(offset, data, options, dbg);
  }

  IOStatus Read(uint64_t offset, size_t n, const IOOptions& options,
                Slice* result, char* scratch,
                IODebugContext* dbg) const override {
    return file_->Read(offset, n, options, result, scratch, dbg);
  }

  IOStatus Close(const IOOptions& options, IODebugContext* dbg) override {
    return file_->Fsync(options, dbg);
  }

  IOStatus Flush(const IOOptions& /*options*/,
                 IODebugContext* /*dbg*/) override {
    return IOStatus::OK();
  }

  IOStatus Sync(const IOOptions& options, IODebugContext* dbg) override {
    return file_->Fsync(options, dbg);
  }

 private:
  MemFile* file_;
};

class MockWritableFile : public FSWritableFile {
 public:
  MockWritableFile(MemFile* file, const FileOptions& opts)
      : file_(file),
        use_direct_io_(opts.use_direct_writes),
        rate_limiter_(opts.rate_limiter) {
    file_->Ref();
  }

  ~MockWritableFile() override { file_->Unref(); }

  bool use_direct_io() const override { return false && use_direct_io_; }

  using FSWritableFile::Append;
  IOStatus Append(const Slice& data, const IOOptions& options,
                  IODebugContext* dbg) override {
    size_t bytes_written = 0;
    while (bytes_written < data.size()) {
      auto bytes = RequestToken(data.size() - bytes_written);
      IOStatus s = file_->Append(Slice(data.data() + bytes_written, bytes),
                                 options, dbg);
      if (!s.ok()) {
        return s;
      }
      bytes_written += bytes;
    }
    return IOStatus::OK();
  }
  async_result AsyncAppend(const Slice& data, const IOOptions& options,
                               IODebugContext* dbg) override {
    (void)data;
    (void)options;
    (void)dbg;
    throw "Not implemented";
  }
  async_result AsyncAppend(const Slice& data, const IOOptions& options,
                           const DataVerificationInfo& /* verification_info */,
                           IODebugContext* dbg) override {
    (void)data;
    (void)options;
    (void)dbg;
    throw "Not implemented";
  }

  using FSWritableFile::PositionedAppend;
  IOStatus PositionedAppend(const Slice& data, uint64_t /*offset*/,
                            const IOOptions& options,
                            IODebugContext* dbg) override {
    assert(use_direct_io_);
    return Append(data, options, dbg);
  }

  IOStatus Truncate(uint64_t size, const IOOptions& options,
                    IODebugContext* dbg) override {
    file_->Truncate(static_cast<size_t>(size), options, dbg);
    return IOStatus::OK();
  }
  IOStatus Close(const IOOptions& options, IODebugContext* dbg) override {
    return file_->Fsync(options, dbg);
  }

  IOStatus Flush(const IOOptions& /*options*/,
                 IODebugContext* /*dbg*/) override {
    return IOStatus::OK();
  }

  IOStatus Sync(const IOOptions& options, IODebugContext* dbg) override {
    return file_->Fsync(options, dbg);
  }

  uint64_t GetFileSize(const IOOptions& /*options*/,
                       IODebugContext* /*dbg*/) override {
    return file_->Size();
  }

 private:
  inline size_t RequestToken(size_t bytes) {
    if (rate_limiter_ && io_priority_ < Env::IO_TOTAL) {
      bytes = std::min(
          bytes, static_cast<size_t>(rate_limiter_->GetSingleBurstBytes()));
      rate_limiter_->Request(bytes, io_priority_);
    }
    return bytes;
  }

  MemFile* file_;
  bool use_direct_io_;
  RateLimiter* rate_limiter_;
};

class MockEnvDirectory : public FSDirectory {
 public:
  IOStatus Fsync(const IOOptions& /*options*/,
                 IODebugContext* /*dbg*/) override {
    return IOStatus::OK();
  }
};

class MockEnvFileLock : public FileLock {
 public:
  explicit MockEnvFileLock(const std::string& fname) : fname_(fname) {}

  std::string FileName() const { return fname_; }

 private:
  const std::string fname_;
};

class TestMemLogger : public Logger {
 private:
  std::unique_ptr<FSWritableFile> file_;
  std::atomic_size_t log_size_;
  static const uint64_t flush_every_seconds_ = 5;
  std::atomic_uint_fast64_t last_flush_micros_;
  Env* env_;
  IOOptions options_;
  IODebugContext* dbg_;
  std::atomic<bool> flush_pending_;

 public:
  TestMemLogger(std::unique_ptr<FSWritableFile> f, Env* env,
                const IOOptions& options, IODebugContext* dbg,
                const InfoLogLevel log_level = InfoLogLevel::ERROR_LEVEL)
      : Logger(log_level),
        file_(std::move(f)),
        log_size_(0),
        last_flush_micros_(0),
        env_(env),
        options_(options),
        dbg_(dbg),
        flush_pending_(false) {}
  ~TestMemLogger() override {}

  void Flush() override {
    if (flush_pending_) {
      flush_pending_ = false;
    }
    last_flush_micros_ = env_->NowMicros();
  }

  using Logger::Logv;
  void Logv(const char* format, va_list ap) override {
    // We try twice: the first time with a fixed-size stack allocated buffer,
    // and the second time with a much larger dynamically allocated buffer.
    char buffer[500];
    for (int iter = 0; iter < 2; iter++) {
      char* base;
      int bufsize;
      if (iter == 0) {
        bufsize = sizeof(buffer);
        base = buffer;
      } else {
        bufsize = 30000;
        base = new char[bufsize];
      }
      char* p = base;
      char* limit = base + bufsize;

      struct timeval now_tv;
      gettimeofday(&now_tv, nullptr);
      const time_t seconds = now_tv.tv_sec;
      struct tm t;
      memset(&t, 0, sizeof(t));
      struct tm* ret __attribute__((__unused__));
      ret = localtime_r(&seconds, &t);
      assert(ret);
      p += snprintf(p, limit - p, "%04d/%02d/%02d-%02d:%02d:%02d.%06d ",
                    t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
                    t.tm_min, t.tm_sec, static_cast<int>(now_tv.tv_usec));

      // Print the message
      if (p < limit) {
        va_list backup_ap;
        va_copy(backup_ap, ap);
        p += vsnprintf(p, limit - p, format, backup_ap);
        va_end(backup_ap);
      }

      // Truncate to available space if necessary
      if (p >= limit) {
        if (iter == 0) {
          continue;  // Try again with larger buffer
        } else {
          p = limit - 1;
        }
      }

      // Add newline if necessary
      if (p == base || p[-1] != '\n') {
        *p++ = '\n';
      }

      assert(p <= limit);
      const size_t write_size = p - base;

      Status s = file_->Append(Slice(base, write_size), options_, dbg_);
      if (s.ok()) {
        flush_pending_ = true;
        log_size_ += write_size;
      }
      uint64_t now_micros =
          static_cast<uint64_t>(now_tv.tv_sec) * 1000000 + now_tv.tv_usec;
      if (now_micros - last_flush_micros_ >= flush_every_seconds_ * 1000000) {
        flush_pending_ = false;
        last_flush_micros_ = now_micros;
      }
      if (base != buffer) {
        delete[] base;
      }
      break;
    }
  }
  size_t GetLogFileSize() const override { return log_size_; }
};

class MockFileSystem : public FileSystem {
 public:
  explicit MockFileSystem(Env* env, bool supports_direct_io = true)
      : env_(env), supports_direct_io_(supports_direct_io) {}

  ~MockFileSystem() override {
    for (auto i = file_map_.begin(); i != file_map_.end(); ++i) {
      i->second->Unref();
    }
  }

  const char* Name() const override { return "Memory"; }
  IOStatus NewSequentialFile(const std::string& f, const FileOptions& file_opts,
                             std::unique_ptr<FSSequentialFile>* r,
                             IODebugContext* dbg) override;
  IOStatus NewRandomAccessFile(const std::string& f,
                               const FileOptions& file_opts,
                               std::unique_ptr<FSRandomAccessFile>* r,
                               IODebugContext* dbg) override;

  IOStatus NewRandomRWFile(const std::string& fname,
                           const FileOptions& file_opts,
                           std::unique_ptr<FSRandomRWFile>* result,
                           IODebugContext* dbg) override;
  IOStatus ReuseWritableFile(const std::string& fname,
                             const std::string& old_fname,
                             const FileOptions& file_opts,
                             std::unique_ptr<FSWritableFile>* result,
                             IODebugContext* dbg) override;
  IOStatus NewWritableFile(const std::string& fname,
                           const FileOptions& file_opts,
                           std::unique_ptr<FSWritableFile>* result,
                           IODebugContext* dbg) override;
  IOStatus ReopenWritableFile(const std::string& fname,
                              const FileOptions& options,
                              std::unique_ptr<FSWritableFile>* result,
                              IODebugContext* dbg) override;
  IOStatus NewDirectory(const std::string& /*name*/, const IOOptions& io_opts,
                        std::unique_ptr<FSDirectory>* result,
                        IODebugContext* dbg) override;
  IOStatus FileExists(const std::string& fname, const IOOptions& /*io_opts*/,
                      IODebugContext* /*dbg*/) override;
  IOStatus GetChildren(const std::string& dir, const IOOptions& options,
                       std::vector<std::string>* result,
                       IODebugContext* dbg) override;
  IOStatus DeleteFile(const std::string& fname, const IOOptions& options,
                      IODebugContext* dbg) override;
  IOStatus Truncate(const std::string& fname, size_t size,
                    const IOOptions& options, IODebugContext* dbg) override;
  IOStatus CreateDir(const std::string& dirname, const IOOptions& options,
                     IODebugContext* dbg) override;
  IOStatus CreateDirIfMissing(const std::string& dirname,
                              const IOOptions& options,
                              IODebugContext* dbg) override;
  IOStatus DeleteDir(const std::string& dirname, const IOOptions& options,
                     IODebugContext* dbg) override;

  IOStatus GetFileSize(const std::string& fname, const IOOptions& options,
                       uint64_t* file_size, IODebugContext* dbg) override;

  IOStatus GetFileModificationTime(const std::string& fname,
                                   const IOOptions& options,
                                   uint64_t* file_mtime,
                                   IODebugContext* dbg) override;
  IOStatus RenameFile(const std::string& src, const std::string& target,
                      const IOOptions& options, IODebugContext* dbg) override;
  IOStatus LinkFile(const std::string& /*src*/, const std::string& /*target*/,
                    const IOOptions& /*options*/,
                    IODebugContext* /*dbg*/) override;
  IOStatus LockFile(const std::string& fname, const IOOptions& options,
                    FileLock** lock, IODebugContext* dbg) override;
  IOStatus UnlockFile(FileLock* lock, const IOOptions& options,
                      IODebugContext* dbg) override;
  IOStatus GetTestDirectory(const IOOptions& options, std::string* path,
                            IODebugContext* dbg) override;
  IOStatus NewLogger(const std::string& fname, const IOOptions& io_opts,
                     std::shared_ptr<Logger>* result,
                     IODebugContext* dbg) override;
  // Get full directory name for this db.
  IOStatus GetAbsolutePath(const std::string& db_path,
                           const IOOptions& /*options*/,
                           std::string* output_path,
                           IODebugContext* /*dbg*/) override {
    *output_path = NormalizeMockPath(db_path);
    if (output_path->at(0) != '/') {
      return IOStatus::NotSupported("GetAbsolutePath");
    } else {
      return IOStatus::OK();
    }
  }
  IOStatus IsDirectory(const std::string& /*path*/,
                       const IOOptions& /*options*/, bool* /*is_dir*/,
                       IODebugContext* /*dgb*/) override {
    return IOStatus::NotSupported("IsDirectory");
  }

  Status CorruptBuffer(const std::string& fname);

 private:
  bool RenameFileInternal(const std::string& src, const std::string& dest);
  void DeleteFileInternal(const std::string& fname);
  bool GetChildrenInternal(const std::string& fname,
                           std::vector<std::string>* results);

  std::string NormalizeMockPath(const std::string& path) {
    std::string p = NormalizePath(path);
    if (p.back() == kFilePathSeparator && p.size() > 1) {
      p.pop_back();
    }
    return p;
  }

 private:
  // Map from filenames to MemFile objects, representing a simple file system.
  port::Mutex mutex_;
  std::map<std::string, MemFile*> file_map_;  // Protected by mutex_.
  Env* env_;
  bool supports_direct_io_;
};

}  // Anonymous namespace
// Partial implementation of the Env interface.
IOStatus MockFileSystem::NewSequentialFile(
    const std::string& fname, const FileOptions& file_opts,
    std::unique_ptr<FSSequentialFile>* result, IODebugContext* /*dbg*/) {
  auto fn = NormalizeMockPath(fname);

  MutexLock lock(&mutex_);
  if (file_map_.find(fn) == file_map_.end()) {
    *result = nullptr;
    return IOStatus::PathNotFound(fn);
  }
  auto* f = file_map_[fn];
  if (f->is_lock_file()) {
    return IOStatus::InvalidArgument(fn, "Cannot open a lock file.");
  } else if (file_opts.use_direct_reads && !supports_direct_io_) {
    return IOStatus::NotSupported("Direct I/O Not Supported");
  } else {
    result->reset(new MockSequentialFile(f, file_opts));
    return IOStatus::OK();
  }
}

IOStatus MockFileSystem::NewRandomAccessFile(
    const std::string& fname, const FileOptions& file_opts,
    std::unique_ptr<FSRandomAccessFile>* result, IODebugContext* /*dbg*/) {
  auto fn = NormalizeMockPath(fname);
  MutexLock lock(&mutex_);
  if (file_map_.find(fn) == file_map_.end()) {
    *result = nullptr;
    return IOStatus::PathNotFound(fn);
  }
  auto* f = file_map_[fn];
  if (f->is_lock_file()) {
    return IOStatus::InvalidArgument(fn, "Cannot open a lock file.");
  } else if (file_opts.use_direct_reads && !supports_direct_io_) {
    return IOStatus::NotSupported("Direct I/O Not Supported");
  } else {
    result->reset(new MockRandomAccessFile(f, file_opts));
    return IOStatus::OK();
  }
}

IOStatus MockFileSystem::NewRandomRWFile(
    const std::string& fname, const FileOptions& /*file_opts*/,
    std::unique_ptr<FSRandomRWFile>* result, IODebugContext* /*dbg*/) {
  auto fn = NormalizeMockPath(fname);
  MutexLock lock(&mutex_);
  if (file_map_.find(fn) == file_map_.end()) {
    *result = nullptr;
    return IOStatus::PathNotFound(fn);
  }
  auto* f = file_map_[fn];
  if (f->is_lock_file()) {
    return IOStatus::InvalidArgument(fn, "Cannot open a lock file.");
  }
  result->reset(new MockRandomRWFile(f));
  return IOStatus::OK();
}

IOStatus MockFileSystem::ReuseWritableFile(
    const std::string& fname, const std::string& old_fname,
    const FileOptions& options, std::unique_ptr<FSWritableFile>* result,
    IODebugContext* dbg) {
  auto s = RenameFile(old_fname, fname, IOOptions(), dbg);
  if (!s.ok()) {
    return s;
  } else {
    result->reset();
    return NewWritableFile(fname, options, result, dbg);
  }
}

IOStatus MockFileSystem::NewWritableFile(
    const std::string& fname, const FileOptions& file_opts,
    std::unique_ptr<FSWritableFile>* result, IODebugContext* /*dbg*/) {
  auto fn = NormalizeMockPath(fname);
  MutexLock lock(&mutex_);
  if (file_map_.find(fn) != file_map_.end()) {
    DeleteFileInternal(fn);
  }
  MemFile* file = new MemFile(env_, fn, false);
  file->Ref();
  file_map_[fn] = file;
  if (file_opts.use_direct_writes && !supports_direct_io_) {
    return IOStatus::NotSupported("Direct I/O Not Supported");
  } else {
    result->reset(new MockWritableFile(file, file_opts));
    return IOStatus::OK();
  }
}

IOStatus MockFileSystem::ReopenWritableFile(
    const std::string& fname, const FileOptions& file_opts,
    std::unique_ptr<FSWritableFile>* result, IODebugContext* /*dbg*/) {
  auto fn = NormalizeMockPath(fname);
  MutexLock lock(&mutex_);
  MemFile* file = nullptr;
  if (file_map_.find(fn) == file_map_.end()) {
    file = new MemFile(env_, fn, false);
    // Only take a reference when we create the file objectt
    file->Ref();
    file_map_[fn] = file;
  } else {
    file = file_map_[fn];
  }
  if (file_opts.use_direct_writes && !supports_direct_io_) {
    return IOStatus::NotSupported("Direct I/O Not Supported");
  } else {
    result->reset(new MockWritableFile(file, file_opts));
    return IOStatus::OK();
  }
}

IOStatus MockFileSystem::NewDirectory(const std::string& /*name*/,
                                      const IOOptions& /*io_opts*/,
                                      std::unique_ptr<FSDirectory>* result,
                                      IODebugContext* /*dbg*/) {
  result->reset(new MockEnvDirectory());
  return IOStatus::OK();
}

IOStatus MockFileSystem::FileExists(const std::string& fname,
                                    const IOOptions& /*io_opts*/,
                                    IODebugContext* /*dbg*/) {
  auto fn = NormalizeMockPath(fname);
  MutexLock lock(&mutex_);
  if (file_map_.find(fn) != file_map_.end()) {
    // File exists
    return IOStatus::OK();
  }
  // Now also check if fn exists as a dir
  for (const auto& iter : file_map_) {
    const std::string& filename = iter.first;
    if (filename.size() >= fn.size() + 1 && filename[fn.size()] == '/' &&
        Slice(filename).starts_with(Slice(fn))) {
      return IOStatus::OK();
    }
  }
  return IOStatus::NotFound();
}

bool MockFileSystem::GetChildrenInternal(const std::string& dir,
                                         std::vector<std::string>* result) {
  auto d = NormalizeMockPath(dir);
  bool found_dir = false;
  result->clear();
  for (const auto& iter : file_map_) {
    const std::string& filename = iter.first;

    if (filename == d) {
      found_dir = true;
    } else if (filename.size() >= d.size() + 1 && filename[d.size()] == '/' &&
               Slice(filename).starts_with(Slice(d))) {
      found_dir = true;
      size_t next_slash = filename.find('/', d.size() + 1);
      if (next_slash != std::string::npos) {
        result->push_back(
            filename.substr(d.size() + 1, next_slash - d.size() - 1));
      } else {
        result->push_back(filename.substr(d.size() + 1));
      }
    }
  }
  result->erase(std::unique(result->begin(), result->end()), result->end());
  return found_dir;
}

IOStatus MockFileSystem::GetChildren(const std::string& dir,
                                     const IOOptions& /*options*/,
                                     std::vector<std::string>* result,
                                     IODebugContext* /*dbg*/) {
  MutexLock lock(&mutex_);
  bool found_dir = GetChildrenInternal(dir, result);
  return found_dir ? IOStatus::OK() : IOStatus::NotFound(dir);
}

void MockFileSystem::DeleteFileInternal(const std::string& fname) {
  assert(fname == NormalizeMockPath(fname));
  const auto& pair = file_map_.find(fname);
  if (pair != file_map_.end()) {
    pair->second->Unref();
    file_map_.erase(fname);
  }
}

IOStatus MockFileSystem::DeleteFile(const std::string& fname,
                                    const IOOptions& /*options*/,
                                    IODebugContext* /*dbg*/) {
  auto fn = NormalizeMockPath(fname);
  MutexLock lock(&mutex_);
  if (file_map_.find(fn) == file_map_.end()) {
    return IOStatus::PathNotFound(fn);
  }

  DeleteFileInternal(fn);
  return IOStatus::OK();
}

IOStatus MockFileSystem::Truncate(const std::string& fname, size_t size,
                                  const IOOptions& options,
                                  IODebugContext* dbg) {
  auto fn = NormalizeMockPath(fname);
  MutexLock lock(&mutex_);
  auto iter = file_map_.find(fn);
  if (iter == file_map_.end()) {
    return IOStatus::PathNotFound(fn);
  }
  iter->second->Truncate(size, options, dbg);
  return IOStatus::OK();
}

IOStatus MockFileSystem::CreateDir(const std::string& dirname,
                                   const IOOptions& /*options*/,
                                   IODebugContext* /*dbg*/) {
  auto dn = NormalizeMockPath(dirname);
  MutexLock lock(&mutex_);
  if (file_map_.find(dn) == file_map_.end()) {
    MemFile* file = new MemFile(env_, dn, false);
    file->Ref();
    file_map_[dn] = file;
  } else {
    return IOStatus::IOError();
  }
  return IOStatus::OK();
}

IOStatus MockFileSystem::CreateDirIfMissing(const std::string& dirname,
                                            const IOOptions& options,
                                            IODebugContext* dbg) {
  CreateDir(dirname, options, dbg).PermitUncheckedError();
  return IOStatus::OK();
}

IOStatus MockFileSystem::DeleteDir(const std::string& dirname,
                                   const IOOptions& /*options*/,
                                   IODebugContext* /*dbg*/) {
  auto dir = NormalizeMockPath(dirname);
  MutexLock lock(&mutex_);
  if (file_map_.find(dir) == file_map_.end()) {
    return IOStatus::PathNotFound(dir);
  } else {
    std::vector<std::string> children;
    if (GetChildrenInternal(dir, &children)) {
      for (const auto& child : children) {
        DeleteFileInternal(child);
      }
    }
    DeleteFileInternal(dir);
    return IOStatus::OK();
  }
}

IOStatus MockFileSystem::GetFileSize(const std::string& fname,
                                     const IOOptions& /*options*/,
                                     uint64_t* file_size,
                                     IODebugContext* /*dbg*/) {
  auto fn = NormalizeMockPath(fname);
  MutexLock lock(&mutex_);
  auto iter = file_map_.find(fn);
  if (iter == file_map_.end()) {
    return IOStatus::PathNotFound(fn);
  }

  *file_size = iter->second->Size();
  return IOStatus::OK();
}

IOStatus MockFileSystem::GetFileModificationTime(const std::string& fname,
                                                 const IOOptions& /*options*/,
                                                 uint64_t* time,
                                                 IODebugContext* /*dbg*/) {
  auto fn = NormalizeMockPath(fname);
  MutexLock lock(&mutex_);
  auto iter = file_map_.find(fn);
  if (iter == file_map_.end()) {
    return IOStatus::PathNotFound(fn);
  }
  *time = iter->second->ModifiedTime();
  return IOStatus::OK();
}

bool MockFileSystem::RenameFileInternal(const std::string& src,
                                        const std::string& dest) {
  if (file_map_.find(src) == file_map_.end()) {
    return false;
  } else {
    std::vector<std::string> children;
    if (GetChildrenInternal(src, &children)) {
      for (const auto& child : children) {
        RenameFileInternal(src + "/" + child, dest + "/" + child);
      }
    }
    DeleteFileInternal(dest);
    file_map_[dest] = file_map_[src];
    file_map_.erase(src);
    return true;
  }
}

IOStatus MockFileSystem::RenameFile(const std::string& src,
                                    const std::string& dest,
                                    const IOOptions& /*options*/,
                                    IODebugContext* /*dbg*/) {
  auto s = NormalizeMockPath(src);
  auto t = NormalizeMockPath(dest);
  MutexLock lock(&mutex_);
  bool found = RenameFileInternal(s, t);
  if (!found) {
    return IOStatus::PathNotFound(s);
  } else {
    return IOStatus::OK();
  }
}

IOStatus MockFileSystem::LinkFile(const std::string& src,
                                  const std::string& dest,
                                  const IOOptions& /*options*/,
                                  IODebugContext* /*dbg*/) {
  auto s = NormalizeMockPath(src);
  auto t = NormalizeMockPath(dest);
  MutexLock lock(&mutex_);
  if (file_map_.find(s) == file_map_.end()) {
    return IOStatus::PathNotFound(s);
  }

  DeleteFileInternal(t);
  file_map_[t] = file_map_[s];
  file_map_[t]->Ref();  // Otherwise it might get deleted when noone uses s
  return IOStatus::OK();
}

IOStatus MockFileSystem::NewLogger(const std::string& fname,
                                   const IOOptions& io_opts,
                                   std::shared_ptr<Logger>* result,
                                   IODebugContext* dbg) {
  auto fn = NormalizeMockPath(fname);
  MutexLock lock(&mutex_);
  auto iter = file_map_.find(fn);
  MemFile* file = nullptr;
  if (iter == file_map_.end()) {
    file = new MemFile(env_, fn, false);
    file->Ref();
    file_map_[fn] = file;
  } else {
    file = iter->second;
  }
  std::unique_ptr<FSWritableFile> f(new MockWritableFile(file, FileOptions()));
  result->reset(new TestMemLogger(std::move(f), env_, io_opts, dbg));
  return IOStatus::OK();
}

IOStatus MockFileSystem::LockFile(const std::string& fname,
                                  const IOOptions& /*options*/,
                                  FileLock** flock, IODebugContext* /*dbg*/) {
  auto fn = NormalizeMockPath(fname);
  {
    MutexLock lock(&mutex_);
    if (file_map_.find(fn) != file_map_.end()) {
      if (!file_map_[fn]->is_lock_file()) {
        return IOStatus::InvalidArgument(fname, "Not a lock file.");
      }
      if (!file_map_[fn]->Lock()) {
        return IOStatus::IOError(fn, "lock is already held.");
      }
    } else {
      auto* file = new MemFile(env_, fn, true);
      file->Ref();
      file->Lock();
      file_map_[fn] = file;
    }
  }
  *flock = new MockEnvFileLock(fn);
  return IOStatus::OK();
}

IOStatus MockFileSystem::UnlockFile(FileLock* flock,
                                    const IOOptions& /*options*/,
                                    IODebugContext* /*dbg*/) {
  std::string fn = static_cast_with_check<MockEnvFileLock>(flock)->FileName();
  {
    MutexLock lock(&mutex_);
    if (file_map_.find(fn) != file_map_.end()) {
      if (!file_map_[fn]->is_lock_file()) {
        return IOStatus::InvalidArgument(fn, "Not a lock file.");
      }
      file_map_[fn]->Unlock();
    }
  }
  delete flock;
  return IOStatus::OK();
}

IOStatus MockFileSystem::GetTestDirectory(const IOOptions& /*options*/,
                                          std::string* path,
                                          IODebugContext* /*dbg*/) {
  *path = "/test";
  return IOStatus::OK();
}

Status MockFileSystem::CorruptBuffer(const std::string& fname) {
  auto fn = NormalizeMockPath(fname);
  MutexLock lock(&mutex_);
  auto iter = file_map_.find(fn);
  if (iter == file_map_.end()) {
    return Status::IOError(fn, "File not found");
  }
  iter->second->CorruptBuffer();
  return Status::OK();
}
namespace {
class MockSystemClock : public SystemClockWrapper {
 public:
  explicit MockSystemClock(const std::shared_ptr<SystemClock>& c)
      : SystemClockWrapper(c), fake_sleep_micros_(0) {}

  void FakeSleepForMicroseconds(int64_t micros) {
    fake_sleep_micros_.fetch_add(micros);
  }

  const char* Name() const override { return "MockSystemClock"; }

  Status GetCurrentTime(int64_t* unix_time) override {
    auto s = SystemClockWrapper::GetCurrentTime(unix_time);
    if (s.ok()) {
      auto fake_time = fake_sleep_micros_.load() / (1000 * 1000);
      *unix_time += fake_time;
    }
    return s;
  }

  uint64_t NowMicros() override {
    return SystemClockWrapper::NowMicros() + fake_sleep_micros_.load();
  }

  uint64_t NowNanos() override {
    return SystemClockWrapper::NowNanos() + fake_sleep_micros_.load() * 1000;
  }

 private:
  std::atomic<int64_t> fake_sleep_micros_;
};
}  // namespace
MockEnv::MockEnv(Env* base_env)
    : CompositeEnvWrapper(
          base_env, std::make_shared<MockFileSystem>(this),
          std::make_shared<MockSystemClock>(base_env->GetSystemClock())) {}

Status MockEnv::CorruptBuffer(const std::string& fname) {
  auto mock = static_cast_with_check<MockFileSystem>(GetFileSystem().get());
  return mock->CorruptBuffer(fname);
}

void MockEnv::FakeSleepForMicroseconds(int64_t micros) {
  auto mock = static_cast_with_check<MockSystemClock>(GetSystemClock().get());
  mock->FakeSleepForMicroseconds(micros);
}

#ifndef ROCKSDB_LITE
// This is to maintain the behavior before swithcing from InMemoryEnv to MockEnv
Env* NewMemEnv(Env* base_env) { return new MockEnv(base_env); }

#else  // ROCKSDB_LITE

Env* NewMemEnv(Env* /*base_env*/) { return nullptr; }

#endif  // !ROCKSDB_LITE

}  // namespace ROCKSDB_NAMESPACE
