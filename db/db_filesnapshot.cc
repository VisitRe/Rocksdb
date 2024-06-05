//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "db/db_impl/db_impl.h"
#include "db/job_context.h"
#include "db/version_set.h"
#include "file/file_util.h"
#include "file/filename.h"
#include "logging/logging.h"
#include "port/port.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/metadata.h"
#include "rocksdb/types.h"
#include "test_util/sync_point.h"
#include "util/file_checksum_helper.h"
#include "util/mutexlock.h"

namespace ROCKSDB_NAMESPACE {

Status DBImpl::FlushForGetLiveFiles() {
  return DBImpl::FlushAllColumnFamilies(FlushOptions(),
                                        FlushReason::kGetLiveFiles);
}

Status DBImpl::GetLiveFiles(std::vector<std::string>& ret,
                            uint64_t* manifest_file_size, bool flush_memtable) {
  *manifest_file_size = 0;

  mutex_.Lock();

  if (flush_memtable) {
    Status status = FlushForGetLiveFiles();
    if (!status.ok()) {
      mutex_.Unlock();
      ROCKS_LOG_ERROR(immutable_db_options_.info_log, "Cannot Flush data %s\n",
                      status.ToString().c_str());
      return status;
    }
  }

  // Make a set of all of the live table and blob files
  std::vector<uint64_t> live_table_files;
  std::vector<uint64_t> live_blob_files;
  for (auto cfd : *versions_->GetColumnFamilySet()) {
    if (cfd->IsDropped()) {
      continue;
    }
    cfd->current()->AddLiveFiles(&live_table_files, &live_blob_files);
  }

  ret.clear();
  ret.reserve(live_table_files.size() + live_blob_files.size() +
              3);  // for CURRENT + MANIFEST + OPTIONS

  // create names of the live files. The names are not absolute
  // paths, instead they are relative to dbname_.
  for (const auto& table_file_number : live_table_files) {
    ret.emplace_back(MakeTableFileName("", table_file_number));
  }

  for (const auto& blob_file_number : live_blob_files) {
    ret.emplace_back(BlobFileName("", blob_file_number));
  }

  ret.emplace_back(CurrentFileName(""));
  ret.emplace_back(DescriptorFileName("", versions_->manifest_file_number()));
  // The OPTIONS file number is zero in read-write mode when OPTIONS file
  // writing failed and the DB was configured with
  // `fail_if_options_file_error == false`. In read-only mode the OPTIONS file
  // number is zero when no OPTIONS file exist at all. In those cases we do not
  // record any OPTIONS file in the live file list.
  if (versions_->options_file_number() != 0) {
    ret.emplace_back(OptionsFileName("", versions_->options_file_number()));
  }

  // find length of manifest file while holding the mutex lock
  *manifest_file_size = versions_->manifest_file_size();

  mutex_.Unlock();
  return Status::OK();
}

Status DBImpl::GetSortedWalFiles(VectorWalPtr& files) {
  // Record tracked WALs as a (minimum) cross-check for directory scan
  std::vector<uint64_t> required_by_manifest;

  // If caller disabled deletions, this function should return files that are
  // guaranteed not to be deleted until deletions are re-enabled. We need to
  // wait for pending purges to finish since WalManager doesn't know which
  // files are going to be purged. Additional purges won't be scheduled as
  // long as deletions are disabled (so the below loop must terminate).
  // Also note that we disable deletions anyway to avoid the case where a
  // file is deleted in the middle of the scan, causing IO error.
  Status deletions_disabled = DisableFileDeletions();
  {
    InstrumentedMutexLock l(&mutex_);
    while (pending_purge_obsolete_files_ > 0 || bg_purge_scheduled_ > 0) {
      bg_cv_.Wait();
    }

    // Record tracked WALs as a (minimum) cross-check for directory scan
    const auto& manifest_wals = versions_->GetWalSet().GetWals();
    required_by_manifest.reserve(manifest_wals.size());
    for (const auto& wal : manifest_wals) {
      required_by_manifest.push_back(wal.first);
    }
  }

  Status s = wal_manager_.GetSortedWalFiles(files);

  // DisableFileDeletions / EnableFileDeletions not supported in read-only DB
  if (deletions_disabled.ok()) {
    Status s2 = EnableFileDeletions();
    assert(s2.ok());
    s2.PermitUncheckedError();
  } else {
    assert(deletions_disabled.IsNotSupported());
  }

  if (s.ok()) {
    // Verify includes those required by manifest (one sorted list is superset
    // of the other)
    auto required = required_by_manifest.begin();
    auto included = files.begin();

    while (required != required_by_manifest.end()) {
      if (included == files.end() || *required < (*included)->LogNumber()) {
        // FAIL - did not find
        return Status::Corruption(
            "WAL file " + std::to_string(*required) +
            " required by manifest but not in directory list");
      }
      if (*required == (*included)->LogNumber()) {
        ++required;
        ++included;
      } else {
        assert(*required > (*included)->LogNumber());
        ++included;
      }
    }
  }

  if (s.ok()) {
    size_t wal_count = files.size();
    ROCKS_LOG_INFO(immutable_db_options_.info_log,
                   "Number of WAL files %" ROCKSDB_PRIszt " (%" ROCKSDB_PRIszt
                   " required by manifest)",
                   wal_count, required_by_manifest.size());
#ifndef NDEBUG
    std::ostringstream wal_names;
    for (const auto& wal : files) {
      wal_names << wal->PathName() << " ";
    }

    std::ostringstream wal_required_by_manifest_names;
    for (const auto& wal : required_by_manifest) {
      wal_required_by_manifest_names << wal << ".log ";
    }

    ROCKS_LOG_INFO(immutable_db_options_.info_log,
                   "Log files : %s .Log files required by manifest: %s.",
                   wal_names.str().c_str(),
                   wal_required_by_manifest_names.str().c_str());
#endif  // NDEBUG
  }
  return s;
}

Status DBImpl::GetCurrentWalFile(std::unique_ptr<WalFile>* current_log_file) {
  uint64_t current_logfile_number;
  {
    InstrumentedMutexLock l(&mutex_);
    current_logfile_number = logfile_number_;
  }

  return wal_manager_.GetLiveWalFile(current_logfile_number, current_log_file);
}

Status DBImpl::GetLiveFilesStorageInfo(
    const LiveFilesStorageInfoOptions& opts,
    std::vector<LiveFileStorageInfo>* files) {
  // To avoid returning partial results, only move results to files on success.
  assert(files);
  files->clear();
  std::vector<LiveFileStorageInfo> results;

  // NOTE: This implementation was largely migrated from Checkpoint.

  Status s;
  VectorWalPtr live_wal_files;
  bool flush_memtable = true;
  if (!immutable_db_options_.allow_2pc) {
    if (opts.wal_size_for_flush == std::numeric_limits<uint64_t>::max()) {
      flush_memtable = false;
    } else if (opts.wal_size_for_flush > 0) {
      // If the outstanding WAL files are small, we skip the flush.
      s = GetSortedWalFiles(live_wal_files);

      if (!s.ok()) {
        return s;
      }

      // Don't flush column families if total log size is smaller than
      // log_size_for_flush. We copy the log files instead.
      // We may be able to cover 2PC case too.
      uint64_t total_wal_size = 0;
      for (auto& wal : live_wal_files) {
        total_wal_size += wal->SizeFileBytes();
      }
      if (total_wal_size < opts.wal_size_for_flush) {
        flush_memtable = false;
      }
      live_wal_files.clear();
    }
  }

  // This is a modified version of GetLiveFiles, to get access to more
  // metadata.
  mutex_.Lock();
  if (flush_memtable) {
    bool wal_locked = lock_wal_count_ > 0;
    if (wal_locked) {
      ROCKS_LOG_INFO(immutable_db_options_.info_log,
                     "Can't FlushForGetLiveFiles while WAL is locked");
    } else {
      Status status = FlushForGetLiveFiles();
      if (!status.ok()) {
        mutex_.Unlock();
        ROCKS_LOG_ERROR(immutable_db_options_.info_log,
                        "Cannot Flush data %s\n", status.ToString().c_str());
        return status;
      }
    }
  }

  // Make a set of all of the live table and blob files
  for (auto cfd : *versions_->GetColumnFamilySet()) {
    if (cfd->IsDropped()) {
      continue;
    }
    VersionStorageInfo& vsi = *cfd->current()->storage_info();
    auto& cf_paths = cfd->ioptions()->cf_paths;

    auto GetDir = [&](size_t path_id) {
      // Matching TableFileName() behavior
      if (path_id >= cf_paths.size()) {
        assert(false);
        return cf_paths.back().path;
      } else {
        return cf_paths[path_id].path;
      }
    };

    for (int level = 0; level < vsi.num_levels(); ++level) {
      const auto& level_files = vsi.LevelFiles(level);
      for (const auto& meta : level_files) {
        assert(meta);

        results.emplace_back();
        LiveFileStorageInfo& info = results.back();

        info.relative_filename = MakeTableFileName(meta->fd.GetNumber());
        info.directory = GetDir(meta->fd.GetPathId());
        info.file_number = meta->fd.GetNumber();
        info.file_type = kTableFile;
        info.size = meta->fd.GetFileSize();
        if (opts.include_checksum_info) {
          info.file_checksum_func_name = meta->file_checksum_func_name;
          info.file_checksum = meta->file_checksum;
          if (info.file_checksum_func_name.empty()) {
            info.file_checksum_func_name = kUnknownFileChecksumFuncName;
            info.file_checksum = kUnknownFileChecksum;
          }
        }
        info.temperature = meta->temperature;
      }
    }
    const auto& blob_files = vsi.GetBlobFiles();
    for (const auto& meta : blob_files) {
      assert(meta);

      results.emplace_back();
      LiveFileStorageInfo& info = results.back();

      info.relative_filename = BlobFileName(meta->GetBlobFileNumber());
      info.directory = GetDir(/* path_id */ 0);
      info.file_number = meta->GetBlobFileNumber();
      info.file_type = kBlobFile;
      info.size = meta->GetBlobFileSize();
      if (opts.include_checksum_info) {
        info.file_checksum_func_name = meta->GetChecksumMethod();
        info.file_checksum = meta->GetChecksumValue();
        if (info.file_checksum_func_name.empty()) {
          info.file_checksum_func_name = kUnknownFileChecksumFuncName;
          info.file_checksum = kUnknownFileChecksum;
        }
      }
      // TODO?: info.temperature
    }
  }

  // Capture some final info before releasing mutex
  const uint64_t manifest_number = versions_->manifest_file_number();
  const uint64_t manifest_size = versions_->manifest_file_size();
  const uint64_t options_number = versions_->options_file_number();
  const uint64_t options_size = versions_->options_file_size_;
  const uint64_t min_log_num = MinLogNumberToKeep();

  // If there is an active log writer, capture current log number and its
  // current size (excluding incomplete records at the log tail), in order to
  // return size of the current WAL file in a consistent state.
  log_write_mutex_.Lock();
  const uint64_t current_log_num = logfile_number_;
  // With `manual_wal_flush` enabled, this function can return size of the file,
  // including yet not flushed data.
  // But we're calling `FlushWAL()` below, so it will be flushed and actual
  // size of the WAL file will be greater or equal than the one we capture here.
  const uint64_t current_log_aligned_len =
      logs_.empty() ? 0
                    : logs_.back().writer->get_latest_complete_record_offset();
  log_write_mutex_.Unlock();

  mutex_.Unlock();

  std::string manifest_fname = DescriptorFileName(manifest_number);
  {  // MANIFEST
    results.emplace_back();
    LiveFileStorageInfo& info = results.back();

    info.relative_filename = manifest_fname;
    info.directory = GetName();
    info.file_number = manifest_number;
    info.file_type = kDescriptorFile;
    info.size = manifest_size;
    info.trim_to_size = true;
    if (opts.include_checksum_info) {
      info.file_checksum_func_name = kUnknownFileChecksumFuncName;
      info.file_checksum = kUnknownFileChecksum;
    }
  }

  {  // CURRENT
    results.emplace_back();
    LiveFileStorageInfo& info = results.back();

    info.relative_filename = kCurrentFileName;
    info.directory = GetName();
    info.file_type = kCurrentFile;
    // CURRENT could be replaced so we have to record the contents as needed.
    info.replacement_contents = manifest_fname + "\n";
    info.size = manifest_fname.size() + 1;
    if (opts.include_checksum_info) {
      info.file_checksum_func_name = kUnknownFileChecksumFuncName;
      info.file_checksum = kUnknownFileChecksum;
    }
  }

  // The OPTIONS file number is zero in read-write mode when OPTIONS file
  // writing failed and the DB was configured with
  // `fail_if_options_file_error == false`. In read-only mode the OPTIONS file
  // number is zero when no OPTIONS file exist at all. In those cases we do not
  // record any OPTIONS file in the live file list.
  if (options_number != 0) {
    results.emplace_back();
    LiveFileStorageInfo& info = results.back();

    info.relative_filename = OptionsFileName(options_number);
    info.directory = GetName();
    info.file_number = options_number;
    info.file_type = kOptionsFile;
    info.size = options_size;
    if (opts.include_checksum_info) {
      info.file_checksum_func_name = kUnknownFileChecksumFuncName;
      info.file_checksum = kUnknownFileChecksum;
    }
  }

  // Some legacy testing stuff  TODO: carefully clean up obsolete parts
  TEST_SYNC_POINT("CheckpointImpl::CreateCheckpoint:FlushDone");

  TEST_SYNC_POINT("CheckpointImpl::CreateCheckpoint:SavedLiveFiles1");
  TEST_SYNC_POINT("CheckpointImpl::CreateCheckpoint:SavedLiveFiles2");

  if (s.ok()) {
    // To maximize the effectiveness of track_and_verify_wals_in_manifest,
    // sync WAL when it is enabled.
    s = FlushWAL(
        immutable_db_options_.track_and_verify_wals_in_manifest /* sync */);
    if (s.IsNotSupported()) {  // read-only DB or similar
      s = Status::OK();
    }
  }

  TEST_SYNC_POINT("CheckpointImpl::CreateCustomCheckpoint:AfterGetLive1");
  TEST_SYNC_POINT("CheckpointImpl::CreateCustomCheckpoint:AfterGetLive2");

  // If we have more than one column family, we also need to get WAL files.
  if (s.ok()) {
    s = GetSortedWalFiles(live_wal_files);
  }
  if (!s.ok()) {
    return s;
  }

  TEST_SYNC_POINT("DBImpl::GetLiveFilesStorageInfo:AfterGettingLiveWalFiles");

  size_t wal_count = live_wal_files.size();
  // Link WAL files. Copy exact size of last one because it is the only one
  // that has changes after the last flush.
  auto wal_dir = immutable_db_options_.GetWalDir();
  for (size_t i = 0; s.ok() && i < wal_count; ++i) {
    const uint64_t log_num = live_wal_files[i]->LogNumber();
    // Indicates whether this is a new WAL, created after we've captured current
    // log number under the mutex.
    const bool new_wal = current_log_num != 0 && log_num > current_log_num;
    if ((live_wal_files[i]->Type() == kAliveLogFile) &&
        (!flush_memtable || log_num >= min_log_num) && !new_wal) {
      results.emplace_back();
      LiveFileStorageInfo& info = results.back();
      auto f = live_wal_files[i]->PathName();
      assert(!f.empty() && f[0] == '/');
      info.relative_filename = f.substr(1);
      info.directory = wal_dir;
      info.file_number = log_num;
      info.file_type = kWalFile;

      if (current_log_num == info.file_number) {
        // Data can be written into the current log file while we're taking a
        // checkpoint, so we need to copy it and trim its size to the consistent
        // state, captured under the mutex.
        info.size = current_log_aligned_len;
        info.trim_to_size = true;
      } else {
        info.size = live_wal_files[i]->SizeFileBytes();
        // Trim the log if log file recycling is enabled. In this case, a hard
        // link doesn't prevent the file from being renamed and recycled.
        // So we need to copy it instead.
        info.trim_to_size = immutable_db_options_.recycle_log_file_num > 0;
      }

      if (opts.include_checksum_info) {
        info.file_checksum_func_name = kUnknownFileChecksumFuncName;
        info.file_checksum = kUnknownFileChecksum;
      }
    }
  }

  if (s.ok()) {
    // Only move results to output on success.
    *files = std::move(results);
  }
  return s;
}

}  // namespace ROCKSDB_NAMESPACE
